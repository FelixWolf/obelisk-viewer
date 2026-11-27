// Impostors (Scene::impostors): pictures of avatars taken into slots of two
// texture arrays (color and depth), and the cards that show them.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <vector>

namespace viewer::render
{

namespace
{

// Must match impostor.vert and impostor.frag.
struct ImpostorVS
{
    glm::mat4 viewProjection;
    glm::vec4 center;
    glm::vec4 right;
    glm::vec4 up;
};
struct ImpostorFS
{
    glm::mat4 inverseCapture;
    glm::mat4 viewProjection;
    glm::vec4 offset;
    glm::vec4 slot;
};

// Which array (0 big, 1 small) and layer a slot is.
std::pair<int, uint32_t> slotPlace(uint32_t slot)
{
    return slot < kImpostorBigSlots ? std::pair(0, slot) : std::pair(1, slot - kImpostorBigSlots);
}

bool blended(const ObjectDraw& draw)
{
    const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
    const auto* pbr = std::get_if<PbrMaterial>(&draw.material);
    return (bp && bp->blend) || (pbr && pbr->blend);
}

bool masked(const ObjectDraw& draw)
{
    const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
    const auto* pbr = std::get_if<PbrMaterial>(&draw.material);
    return (bp && bp->alphaCutoff >= 0.0f) || (pbr && pbr->alphaCutoff >= 0.0f);
}

} // namespace

bool Renderer::createImpostorTargets()
{
    for(int i = 0; i < 2; i++)
    {
        const uint32_t size = i == 0 ? kImpostorBigSize : kImpostorSmallSize;
        const uint32_t layers = i == 0 ? kImpostorBigSlots : kImpostorSmallSlots;
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = kHdrFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = info.height = size;
        info.layer_count_or_depth = layers;
        info.num_levels = 1;
        impostorColor_[i] = SDL_CreateGPUTexture(device_, &info);
        info.format = depthFormat_;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        impostorDepth_[i] = SDL_CreateGPUTexture(device_, &info);
        if(!impostorColor_[i] || !impostorDepth_[i])
        {
            log::error("creating the impostor pictures failed: {}", SDL_GetError());
            impostorTargetsFailed_ = true;
            return false;
        }
    }
    impostorTaken_.fill(false);
    return true;
}

void Renderer::captureImpostors(SDL_GPUCommandBuffer* commands, const Scene& scene,
                                const FrameFS& frameFs, bool drawsReady)
{
    impostorsCaptured_ = 0;
    const bool any = std::ranges::any_of(scene.impostors, &Impostor::capture);
    if(!drawsReady || !any)
    {
        return;
    }
    if(!impostorColor_[0] && (impostorTargetsFailed_ || !createImpostorTargets()))
    {
        return;
    }

    // Each slot's draws, in scene order.
    std::array<std::vector<const ObjectDraw*>, kImpostorSlots> bySlot;
    for(const ObjectDraw& draw : scene.objects)
    {
        if(draw.impostor < 0 || static_cast<uint32_t>(draw.impostor) >= kImpostorSlots ||
           !draw.mesh || !*draw.mesh || draw.mesh->layout == VertexLayout::Terrain || draw.hud ||
           draw.pickOnly || (draw.mesh->layout == VertexLayout::Skinned && draw.joints.empty()))
        {
            continue;
        }
        bySlot[static_cast<size_t>(draw.impostor)].push_back(&draw);
    }

    const Lighting& light = scene.lighting;
    for(const Impostor& impostor : scene.impostors)
    {
        if(!impostor.capture || impostor.slot >= kImpostorSlots)
        {
            continue;
        }
        // Opaque (wholly, then masked; by attachment order), then blended
        // faces far to near, then blended rigged mesh by attachment order
        // (higher layers first), as the view orders them.
        const std::vector<const ObjectDraw*>& draws = bySlot[impostor.slot];
        const glm::vec3 axis = glm::normalize(impostor.center - impostor.eye);
        std::vector<const ObjectDraw*> opaque;
        std::vector<std::pair<float, const ObjectDraw*>> faces;
        std::vector<const ObjectDraw*> rigged;
        for(const ObjectDraw* draw : draws)
        {
            if(!blended(*draw))
            {
                opaque.push_back(draw);
            }
            else if(draw->mesh->layout == VertexLayout::Skinned)
            {
                rigged.push_back(draw);
            }
            else
            {
                const glm::vec3 local = (draw->mesh->boundsMin + draw->mesh->boundsMax) * 0.5f;
                const glm::vec3 at(draw->transform * glm::vec4(local, 1.0f));
                faces.emplace_back(glm::dot(at - impostor.eye, axis), draw);
            }
        }
        std::ranges::stable_sort(opaque,
                                 [](const ObjectDraw* a, const ObjectDraw* b)
                                 {
                                     if(masked(*a) != masked(*b))
                                     {
                                         return !masked(*a);
                                     }
                                     return a->layer < b->layer;
                                 });
        std::ranges::stable_sort(faces,
                                 [](const auto& a, const auto& b) { return a.first > b.first; });
        std::ranges::stable_sort(rigged, [](const ObjectDraw* a, const ObjectDraw* b)
                                 { return a->layer > b->layer; });
        std::vector<const ObjectDraw*> order = std::move(opaque);
        for(const auto& f : faces)
        {
            order.push_back(f.second);
        }
        order.insert(order.end(), rigged.begin(), rigged.end());

        instanceOrder_.clear();
        for(const ObjectDraw* draw : order)
        {
            instanceOrder_.push_back(static_cast<uint32_t>(draw - scene.objects.data()));
        }
        const auto instanceBase = pushInstances(commands, instanceOrder_);

        FrameFS fs = frameFs;
        fs.cameraPosition = glm::vec4(impostor.eye, 1.0f);
        fs.viewAxis = glm::vec4(axis, 0.0f);
        const FrameVS vs{impostor.captureViewProjection, glm::vec4(impostor.eye, 1.0f)};

        const auto [array, layer] = slotPlace(impostor.slot);
        SDL_GPUColorTargetInfo color = {};
        color.texture = impostorColor_[array];
        color.layer_or_depth_plane = layer;
        // The sky's color: what a filtered edge texel blends toward.
        color.clear_color = SDL_FColor{light.skyColor.r, light.skyColor.g, light.skyColor.b, 0.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = impostorDepth_[array];
        depth.layer = static_cast<Uint8>(layer);
        depth.clear_depth = 1.0f;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
        // No clusters (not the view): every light and probe.
        ScenePass scenePass(commands, pass, pipelines_->scene[0], vs, fs, storageOr(lightBuffer_),
                            storageOr(probeBuffer_), emptyStorage_.get(), true);
        drawObjects(scenePass, scene, order, instanceBase);
        SDL_EndGPURenderPass(pass);
        impostorTaken_[impostor.slot] = true;
        impostorsCaptured_++;
    }
}

void Renderer::drawImpostors(ScenePass& pass, const Scene& scene, const glm::mat4& viewProjection)
{
    SDL_GPUGraphicsPipeline* pipeline = pass.pipelines().impostor;
    if(!pipeline || !impostorColor_[0] || scene.impostors.empty())
    {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass.pass(), pipeline);
    pass.forgetPipeline();
    SDL_GPUSampler* sampler = pipelines_->clampSampler;
    const SDL_GPUTextureSamplerBinding pictures[] = {{impostorColor_[0], sampler},
                                                     {impostorDepth_[0], sampler},
                                                     {impostorColor_[1], sampler},
                                                     {impostorDepth_[1], sampler}};
    pass.bindSamplers(pictures);
    for(const Impostor& impostor : scene.impostors)
    {
        if(impostor.slot >= kImpostorSlots || !impostorTaken_[impostor.slot])
        {
            continue;
        }
        const auto [array, layer] = slotPlace(impostor.slot);
        pushVertexUniforms(
            pass.commands(), 0,
            ImpostorVS{viewProjection, glm::vec4(impostor.center + impostor.offset, 1.0f),
                       glm::vec4(impostor.right, 0.0f), glm::vec4(impostor.up, 0.0f)});
        pushFragmentUniforms(
            pass.commands(), 0,
            ImpostorFS{glm::inverse(impostor.captureViewProjection), viewProjection,
                       glm::vec4(impostor.offset, 0.0f),
                       glm::vec4(static_cast<float>(layer), array == 0 ? 1.0f : 0.0f, 0.0f, 0.0f)});
        SDL_DrawGPUPrimitives(pass.pass(), 6, 1, 0, 0);
    }
}

} // namespace viewer::render
