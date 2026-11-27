// HUD attachments: drawn into an image of their own through an orthographic
// camera in HUD space, then shown over the view or on a panel in the scene
// (render::Hud).

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <vector>

namespace viewer::render
{

namespace
{

// HUD objects stay within a few meters of their points (the official
// max_attachment_offset is 2): depth reaches this far either way.
constexpr float kHudDepth = 16.0f;

struct PanelVS
{
    glm::mat4 viewProjection;
    glm::mat4 panel;
    glm::vec4 size;
};

} // namespace

glm::mat4 Renderer::hudViewProjection(const Hud& hud)
{
    // Looking along +X with +Z up: +Y is left, as HUD space has it.
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const float half = 0.5f / std::max(hud.zoom, 0.01f);
    return glm::ortho(-half * hud.aspect, half * hud.aspect, -half, half, -kHudDepth, kHudDepth) *
           view;
}

bool Renderer::drawHud(SDL_GPUCommandBuffer* commands, const Scene& scene, uint32_t width,
                       uint32_t height, bool drawsReady)
{
    if(!drawsReady || width == 0 || height == 0)
    {
        return false;
    }
    // Opaque first, then blended ones from far (larger X) to near.
    std::vector<const ObjectDraw*> order;
    std::vector<const ObjectDraw*> blended;
    for(const ObjectDraw& draw : scene.objects)
    {
        if(!draw.hud || draw.pickOnly || draw.impostor >= 0 || !draw.mesh || !*draw.mesh ||
           draw.mesh->layout != VertexLayout::Object)
        {
            continue;
        }
        const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
        const auto* pbr = std::get_if<PbrMaterial>(&draw.material);
        ((bp && bp->blend) || (pbr && pbr->blend) ? blended : order).push_back(&draw);
    }
    if(order.empty() && blended.empty())
    {
        return false;
    }
    std::ranges::sort(blended, [](const ObjectDraw* a, const ObjectDraw* b)
                      { return a->transform[3].x > b->transform[3].x; });
    const size_t opaqueCount = order.size();
    order.insert(order.end(), blended.begin(), blended.end());

    if(!hudImage_ || hudWidth_ != width || hudHeight_ != height)
    {
        for(SDL_GPUTexture* t : {hudImage_, hudDepth_})
        {
            if(t)
            {
                SDL_ReleaseGPUTexture(device_, t);
            }
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.format = kHudFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        hudImage_ = SDL_CreateGPUTexture(device_, &info);
        info.format = depthFormat_;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        hudDepth_ = SDL_CreateGPUTexture(device_, &info);
        hudWidth_ = width;
        hudHeight_ = height;
        if(!hudImage_ || !hudDepth_)
        {
            log::error("creating the {}x{} HUD image failed: {}", width, height, SDL_GetError());
            return false;
        }
    }

    SDL_GPUColorTargetInfo color = {};
    color.texture = hudImage_;
    color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = hudDepth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);

    const Pipelines& p = *pipelines_;
    const FrameVS vs{hudViewProjection(scene.hud), glm::vec4(0.0f)};
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for(size_t i = 0; i < order.size(); i++)
    {
        const ObjectDraw& draw = *order[i];
        const int blend = i >= opaqueCount ? 1 : 0;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            pipeline = p.hudLegacy[blend];
            const SDL_GPUTextureSamplerBinding sampler = {
                texture(m->diffuse, m->diffuseLoading ? loading_ : white_), p.repeatSampler};
            if(pipeline != bound)
            {
                bound = pipeline;
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                pushVertexUniforms(commands, 0, vs);
            }
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            pipeline = p.hudPbr[blend];
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(t->albedo, t->albedoLoading ? loading_ : white_), p.repeatSampler},
                {texture(t->emissive, white_), p.repeatSampler}};
            if(pipeline != bound)
            {
                bound = pipeline;
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                pushVertexUniforms(commands, 0, vs);
            }
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
        }
        // Each pipeline: the draw data, and each draw's index per instance.
        SDL_GPUBuffer* draws = drawBuffer_.buffer.get();
        SDL_BindGPUVertexStorageBuffers(pass, 0, &draws, 1);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &draws, 1);
        const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
        SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);
        const SDL_GPUBufferBinding vertices = {draw.mesh->vertexBuffer(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        const SDL_GPUBufferBinding indices = {draw.mesh->indexBuffer(), 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, draw.mesh->indexCount, 1, draw.mesh->firstIndex,
                                     static_cast<Sint32>(draw.mesh->firstVertex),
                                     static_cast<uint32_t>(&draw - scene.objects.data()));
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

void Renderer::compositeHud(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->hudComposite);
    const SDL_GPUTextureSamplerBinding hud = {hudImage_, pipelines_->clampSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &hud, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void Renderer::drawHudPanel(ScenePass& pass, const Hud& hud, const glm::mat4& viewProjection)
{
    SDL_BindGPUGraphicsPipeline(pass.pass(), pass.pipelines().hudPanel);
    pass.forgetPipeline();
    const SDL_GPUTextureSamplerBinding image[] = {{hudImage_, pipelines_->clampSampler}};
    pass.bindSamplers(image);
    pushVertexUniforms(
        pass.commands(), 0,
        PanelVS{viewProjection, hud.panel,
                glm::vec4(hud.panelHeight * hud.aspect, hud.panelHeight, 0.0f, 0.0f)});
    SDL_DrawGPUPrimitives(pass.pass(), 6, 1, 0, 0);
}

} // namespace viewer::render
