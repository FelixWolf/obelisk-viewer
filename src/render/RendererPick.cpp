// Picking: what a ray hits, found by drawing one pixel straight down it
// (and one at the HUD point) with pick.frag, then reading the pixels back
// once the GPU is done with them, without waiting for it.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <vector>

namespace viewer::render
{

namespace
{

// A pixel's worth of the ray: the narrowest view that still rasterizes.
constexpr float kPickFov = 1e-3f;
constexpr float kPickNear = 0.05f;
// HUD points: a square this wide in HUD units.
constexpr float kHudPickSize = 1e-4f;
constexpr float kHudDepth = 16.0f;

constexpr uint32_t kPickTargets = 4;
// The world and the HUD as seen, then each counting every face whatever
// its alpha (pick.frag knows these by their x).
constexpr uint32_t kPickPixels = 4;
constexpr uint32_t kPickTexelBytes = 16; // RGBA32
constexpr uint32_t kPickTargetBytes = kPickPixels * kPickTexelBytes;

} // namespace

void Renderer::drawPick(SDL_GPUCommandBuffer* commands, const Scene& scene, bool drawsReady)
{
    if(!scene.pick || pickInFlight_ || pickSubmitted_ || !drawsReady)
    {
        return;
    }
    const PickRequest& request = *scene.pick;
    if(!pickTargets_[0])
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = kPickPixels;
        info.height = 1;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        for(uint32_t i = 0; i < kPickTargets; i++)
        {
            info.format = i == 0 ? SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT
                                 : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            pickTargets_[i] = SDL_CreateGPUTexture(device_, &info);
        }
        info.format = depthFormat_;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        pickDepth_ = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo transfer = {};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        transfer.size = kPickTargets * kPickTargetBytes;
        pickTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer);
        if(!pickTargets_[1] || !pickTargets_[2] || !pickTargets_[3] || !pickDepth_ ||
           !pickTransfer_)
        {
            log::error("creating the pick targets failed: {}", SDL_GetError());
            return;
        }
    }

    // The world: a narrow view down the ray, drawing what could be there.
    const glm::vec3 dir = glm::normalize(request.direction);
    const glm::vec3 up =
        std::abs(dir.z) < 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::mat4 worldView = glm::perspective(kPickFov, 1.0f, kPickNear, request.maxDistance) *
                                glm::lookAt(request.origin, request.origin + dir, up);
    const Frustum worldFrustum = Frustum::fromViewProjection(worldView);
    // The HUD: a tiny orthographic square around the point (HUD space: the
    // view's X is -Y, its Y is Z).
    glm::mat4 hudView(1.0f);
    if(request.hudPoint)
    {
        const glm::vec2 p = *request.hudPoint;
        hudView =
            glm::ortho(-p.x - kHudPickSize, -p.x + kHudPickSize, p.y - kHudPickSize,
                       p.y + kHudPickSize, -kHudDepth, kHudDepth) *
            glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    SDL_GPUColorTargetInfo colors[kPickTargets] = {};
    for(uint32_t i = 0; i < kPickTargets; i++)
    {
        colors[i].texture = pickTargets_[i];
        colors[i].clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        colors[i].load_op = SDL_GPU_LOADOP_CLEAR;
        colors[i].store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = pickDepth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, colors, kPickTargets, &depth);
    SDL_GPUGraphicsPipeline* bound = pipelines_->pick[0];
    SDL_BindGPUGraphicsPipeline(pass, bound);
    SDL_GPUBuffer* draws = drawBuffer_.buffer.get();
    SDL_BindGPUVertexStorageBuffers(pass, 0, &draws, 1);
    SDL_BindGPUFragmentStorageBuffers(pass, 0, &draws, 1);
    const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);

    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    auto drawPixel =
        [&](uint32_t pixel, const glm::mat4& viewProjection, bool hud, bool transparent)
    {
        const SDL_GPUViewport viewport = {static_cast<float>(pixel), 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        const SDL_Rect scissor = {static_cast<int>(pixel), 0, 1, 1};
        SDL_SetGPUScissor(pass, &scissor);
        pushVertexUniforms(commands, 0, FrameVS{viewProjection, glm::vec4(0.0f)});
        for(const ObjectDraw& draw : scene.objects)
        {
            if(draw.pickId == 0 || draw.hud != hud || draw.impostor >= 0 || !draw.mesh ||
               !*draw.mesh ||
               draw.mesh->layout != VertexLayout::Object ||
               (draw.pickTransparentOnly && !transparent))
            {
                continue;
            }
            if(!hud)
            {
                const glm::vec3 localCenter = (draw.mesh->boundsMin + draw.mesh->boundsMax) * 0.5f;
                const glm::mat3 m(draw.transform);
                const glm::mat3 absolute(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
                const glm::vec3 size = absolute * (draw.mesh->boundsMax - draw.mesh->boundsMin);
                if(!worldFrustum.containsSphere(
                       glm::vec3(draw.transform * glm::vec4(localCenter, 1.0f)),
                       0.5f * glm::length(size)))
                {
                    continue;
                }
            }
            const GpuTexture* base = nullptr;
            bool bothSides = false;
            if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
            {
                base = m->diffuse;
            }
            else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
            {
                base = t->albedo;
                bothSides = t->doubleSided;
            }
            // Faces turned away aren't hit, as they aren't drawn.
            SDL_GPUGraphicsPipeline* pipeline = pipelines_->pick[bothSides ? 1 : 0];
            if(pipeline != bound)
            {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound = pipeline;
            }
            const SDL_GPUTextureSamplerBinding sampler = {texture(base, white_),
                                                          pipelines_->repeatSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
            const SDL_GPUBufferBinding vertices = {draw.mesh->vertexBuffer(), 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            const SDL_GPUBufferBinding indices = {draw.mesh->indexBuffer(), 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(pass, draw.mesh->indexCount, 1, draw.mesh->firstIndex,
                                         static_cast<Sint32>(draw.mesh->firstVertex),
                                         static_cast<uint32_t>(&draw - scene.objects.data()));
        }
    };
    drawPixel(0, worldView, false, false);
    drawPixel(2, worldView, false, true);
    if(request.hudPoint)
    {
        drawPixel(1, hudView, true, false);
        drawPixel(3, hudView, true, true);
    }
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    for(uint32_t i = 0; i < kPickTargets; i++)
    {
        SDL_GPUTextureRegion source = {};
        source.texture = pickTargets_[i];
        source.w = kPickPixels;
        source.h = 1;
        source.d = 1;
        SDL_GPUTextureTransferInfo destination = {};
        destination.transfer_buffer = pickTransfer_;
        destination.offset = i * kPickTargetBytes;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
    }
    SDL_EndGPUCopyPass(copy);
    pickSubmitted_ = true;
    pickRequest_ = request.id;
}

void Renderer::readPick()
{
    pickInFlight_ = false;
    const auto* bytes =
        static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device_, pickTransfer_, false));
    if(!bytes)
    {
        return;
    }
    // Target t, pixel p: 16 bytes at t * kPickTargetBytes + p * 16.
    auto read = [&](uint32_t target, uint32_t pixel, auto& out)
    {
        std::memcpy(&out, bytes + target * kPickTargetBytes + pixel * kPickTexelBytes, sizeof(out));
    };
    // The nearest hit of the HUD's pixel, else the world's: HUDs are in
    // front of everything.
    auto hitOf = [&](uint32_t hudPixel, uint32_t worldPixel)
    {
        PickHit hit;
        for(uint32_t pixel : {hudPixel, worldPixel})
        {
            glm::uvec4 id;
            read(0, pixel, id);
            if(id.w == 0)
            {
                continue;
            }
            glm::vec4 position;
            glm::vec4 normal;
            glm::vec4 binormal;
            read(1, pixel, position);
            read(2, pixel, normal);
            read(3, pixel, binormal);
            hit.pickId = static_cast<uint64_t>(id.y) << 32 | id.x;
            hit.hud = pixel == hudPixel;
            hit.position = glm::vec3(position);
            hit.normal = glm::vec3(normal);
            hit.binormal = glm::vec3(binormal);
            hit.uv = glm::vec2(position.w, normal.w);
            float t = 0.0f;
            std::memcpy(&t, &id.z, sizeof(t));
            hit.st = glm::vec2(binormal.w, t);
            break;
        }
        return hit;
    };
    PickResult result;
    static_cast<PickHit&>(result) = hitOf(1, 0);
    result.transparent = hitOf(3, 2);
    result.request = pickRequest_;
    SDL_UnmapGPUTransferBuffer(device_, pickTransfer_);
    pick_ = result;
}

void Renderer::drawOutline(SDL_GPUCommandBuffer* commands, const Scene& scene,
                           SDL_GPUTexture* target, uint32_t width, uint32_t height,
                           const FrameVS& frameVs)
{
    if(width == 0 || height == 0)
    {
        return;
    }
    // The hovered prim in gold (the same pick id, any face); the face with
    // the keyboard in blue (that face only), over it.
    if(scene.highlight != 0)
    {
        drawOutline(commands, scene, target, width, height, frameVs, scene.highlight, false,
                    glm::vec4(1.0f, 0.85f, 0.35f, 0.9f));
    }
    if(scene.focusHighlight != 0)
    {
        drawOutline(commands, scene, target, width, height, frameVs, scene.focusHighlight, true,
                    glm::vec4(0.35f, 0.75f, 1.0f, 0.95f));
    }
}

void Renderer::drawOutline(SDL_GPUCommandBuffer* commands, const Scene& scene,
                           SDL_GPUTexture* target, uint32_t width, uint32_t height,
                           const FrameVS& frameVs, uint64_t pickId, bool faceOnly,
                           const glm::vec4& outlineColor)
{
    std::vector<const ObjectDraw*> draws;
    for(const ObjectDraw& draw : scene.objects)
    {
        const bool same = faceOnly ? draw.pickId == pickId : draw.pickId >> 8 == pickId >> 8;
        if(same && !draw.hud && !draw.pickTransparentOnly && draw.impostor < 0 && draw.mesh &&
           *draw.mesh &&
           draw.mesh->layout == VertexLayout::Object)
        {
            draws.push_back(&draw);
        }
    }
    if(draws.empty())
    {
        return;
    }
    if(!outlineMask_ || outlineWidth_ != width || outlineHeight_ != height)
    {
        if(outlineMask_)
        {
            SDL_ReleaseGPUTexture(device_, outlineMask_);
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        outlineMask_ = SDL_CreateGPUTexture(device_, &info);
        outlineWidth_ = width;
        outlineHeight_ = height;
        if(!outlineMask_)
        {
            log::error("creating the outline mask failed: {}", SDL_GetError());
            return;
        }
    }

    // Its visible pixels, against the view's depth.
    SDL_GPUColorTargetInfo color = {};
    color.texture = outlineMask_;
    color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depthTarget_;
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->outlineMask);
    pushVertexUniforms(commands, 0, frameVs);
    SDL_GPUBuffer* drawData = drawBuffer_.buffer.get();
    SDL_BindGPUVertexStorageBuffers(pass, 0, &drawData, 1);
    const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);
    for(const ObjectDraw* draw : draws)
    {
        const SDL_GPUBufferBinding vertices = {draw->mesh->vertexBuffer(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        const SDL_GPUBufferBinding indices = {draw->mesh->indexBuffer(), 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, draw->mesh->indexCount, 1, draw->mesh->firstIndex,
                                     static_cast<Sint32>(draw->mesh->firstVertex),
                                     static_cast<uint32_t>(draw - scene.objects.data()));
    }
    SDL_EndGPURenderPass(pass);

    // The outline over the view.
    struct OutlineFS
    {
        glm::vec4 color;
        glm::vec4 params;
    };
    SDL_GPUColorTargetInfo view = {};
    view.texture = target;
    view.load_op = SDL_GPU_LOADOP_LOAD;
    view.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(commands, &view, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->outline);
    const SDL_GPUTextureSamplerBinding mask = {outlineMask_, pipelines_->clampSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &mask, 1);
    pushFragmentUniforms(commands, 0, OutlineFS{outlineColor, glm::vec4(2.0f, 0, 0, 0)});
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer::render
