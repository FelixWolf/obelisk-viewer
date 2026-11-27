// Glow, as the official viewer makes it (LLPipeline::generateGlow,
// combineGlow): glowing faces say how much each pixel glows; the view as
// the display will show it, times that, goes into a small image (512 by
// 512, RenderGlowResolutionPow 9), which is blurred across and down twice
// (RenderGlowIterations 2, RenderGlowWidth 1.3, RenderGlowStrength 0.325);
// the tonemap pass adds it over the view.

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

constexpr uint32_t kGlowSize = 512;
constexpr int kBlurPasses = 4; // two each way
constexpr float kGlowWidth = 1.3f;
constexpr float kGlowStrength = 0.325f;

struct GlowFS
{
    glm::vec4 settings; // as TonemapFS's
};

struct BlurFS
{
    glm::vec4 params; // xy step, z strength
};

} // namespace

void Renderer::drawGlow(SDL_GPUCommandBuffer* commands, const Scene& scene, const Lighting& light,
                        uint32_t width, uint32_t height, const FrameVS& frameVs, bool drawsReady)
{
    glowResult_ = nullptr;
    if(!scene.glow || !drawsReady || width == 0 || height == 0)
    {
        return;
    }
    std::vector<const ObjectDraw*> draws;
    for(const ObjectDraw& draw : scene.objects)
    {
        if(draw.glow > 0.0f && !draw.hud && !draw.pickOnly && !draw.shadowOnly &&
           draw.impostor < 0 && draw.mesh && *draw.mesh)
        {
            draws.push_back(&draw);
        }
    }
    const bool particlesGlow = std::ranges::any_of(particleBatches_, &ParticleBatch::glows);
    if(draws.empty() && !particlesGlow)
    {
        return;
    }

    // The mask (the view's size) and the glow images, made when first needed.
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    if(!glowMask_ || glowMaskWidth_ != width || glowMaskHeight_ != height)
    {
        if(glowMask_)
        {
            SDL_ReleaseGPUTexture(device_, glowMask_);
        }
        info.format = kGlowMaskFormat;
        info.width = width;
        info.height = height;
        glowMask_ = SDL_CreateGPUTexture(device_, &info);
        glowMaskWidth_ = width;
        glowMaskHeight_ = height;
    }
    for(SDL_GPUTexture*& image : glowImages_)
    {
        if(!image)
        {
            info.format = kGlowFormat;
            info.width = kGlowSize;
            info.height = kGlowSize;
            image = SDL_CreateGPUTexture(device_, &info);
        }
    }
    if(!glowMask_ || !glowImages_[0] || !glowImages_[1] || !glowImages_[2])
    {
        log::error("creating the glow images failed: {}", SDL_GetError());
        return;
    }

    // How much each pixel glows.
    SDL_GPUColorTargetInfo color = {};
    color.texture = glowMask_;
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
    pushVertexUniforms(commands, 0, frameVs);
    SDL_GPUBuffer* drawData = drawBuffer_.buffer.get();
    SDL_GPUBuffer* vertexStorage[] = {drawData,
                                      jointBuffer_.buffer ? jointBuffer_.buffer.get() : drawData};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, &drawData, 1);
    const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for(const ObjectDraw* draw : draws)
    {
        const bool skinned = draw->mesh->layout == VertexLayout::Skinned;
        if(!skinned && draw->mesh->layout != VertexLayout::Object)
        {
            continue;
        }
        SDL_GPUGraphicsPipeline* pipeline =
            skinned ? pipelines_->glowMaskSkinned : pipelines_->glowMask;
        if(pipeline != bound)
        {
            bound = pipeline;
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_BindGPUVertexStorageBuffers(pass, 0, vertexStorage, skinned ? 2 : 1);
        }
        const GpuTexture* base = nullptr;
        const GpuTexture* emissive = nullptr;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw->material))
        {
            base = m->diffuse;
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw->material))
        {
            base = t->albedo;
            emissive = t->emissive;
        }
        const SDL_GPUTextureSamplerBinding samplers[] = {
            {texture(base, white_), pipelines_->repeatSampler},
            {texture(emissive, white_), pipelines_->repeatSampler}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
        const SDL_GPUBufferBinding vertices = {draw->mesh->vertexBuffer(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        const SDL_GPUBufferBinding indices = {draw->mesh->indexBuffer(), 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, draw->mesh->indexCount, 1, draw->mesh->firstIndex,
                                     static_cast<Sint32>(draw->mesh->firstVertex),
                                     static_cast<uint32_t>(draw - scene.objects.data()));
    }
    drawParticleGlow(pass);
    SDL_EndGPURenderPass(pass);

    // The view as displayed, times the glow, into the glow image.
    auto fullscreen = [&](SDL_GPUTexture* target, SDL_GPUGraphicsPipeline* pipeline,
                          std::span<const SDL_GPUTextureSamplerBinding> inputs)
    {
        SDL_GPUColorTargetInfo out = {};
        out.texture = target;
        out.load_op = SDL_GPU_LOADOP_DONT_CARE;
        out.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(commands, &out, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(p, pipeline);
        SDL_BindGPUFragmentSamplers(p, 0, inputs.data(), static_cast<Uint32>(inputs.size()));
        SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
        SDL_EndGPURenderPass(p);
    };
    pushFragmentUniforms(commands, 0,
                         GlowFS{glm::vec4(light.exposure, light.tonemapMix, 0.0f, 0.0f)});
    const SDL_GPUTextureSamplerBinding extractInputs[] = {{hdrTarget_, pipelines_->clampSampler},
                                                          {exposureMap(), pipelines_->clampSampler},
                                                          {glowMask_, pipelines_->clampSampler}};
    fullscreen(glowImages_[2], pipelines_->glowExtract, extractInputs);

    // Blurred across, down, across, down: from image 2 into 0, then back and
    // forth between 0 and 1, ending in 1.
    const float step = kGlowWidth / static_cast<float>(kGlowSize);
    for(int i = 0; i < kBlurPasses; i++)
    {
        SDL_GPUTexture* source = i == 0 ? glowImages_[2] : glowImages_[(i - 1) % 2];
        const glm::vec2 direction = i % 2 == 0 ? glm::vec2(step, 0.0f) : glm::vec2(0.0f, step);
        pushFragmentUniforms(commands, 0, BlurFS{glm::vec4(direction, kGlowStrength, 0.0f)});
        const SDL_GPUTextureSamplerBinding input = {source, pipelines_->clampSampler};
        fullscreen(glowImages_[i % 2], pipelines_->glowBlur, {&input, 1});
    }
    glowResult_ = glowImages_[(kBlurPasses - 1) % 2];
}

} // namespace viewer::render
