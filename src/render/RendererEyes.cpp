// Stereo: a headset's eyes (drawEyes), and --stereo-benchmark's stand-in
// for them (drawStereo). Each eye is drawn into an eye image in the
// window's format, so every pass of a view works unchanged, then copied to
// where it goes.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>

namespace viewer::render
{

bool Renderer::ensureEyeImages(uint32_t width, uint32_t height)
{
    if(eyeImages_[0] && eyeImages_[1] && eyeWidth_ == width && eyeHeight_ == height)
    {
        return true;
    }
    for(SDL_GPUTexture*& image : eyeImages_)
    {
        if(image)
        {
            SDL_ReleaseGPUTexture(device_, image);
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = swapchainFormat();
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        image = SDL_CreateGPUTexture(device_, &info);
    }
    eyeWidth_ = width;
    eyeHeight_ = height;
    if(!eyeImages_[0] || !eyeImages_[1])
    {
        log::error("creating the eye images failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void Renderer::drawStereo(const Frame& frame, const Scene& scene, uint32_t size, float separation)
{
    if(size == 0 || !ensureEyeImages(size, size))
    {
        return;
    }
    const Camera& center = scene.camera;
    const glm::vec3 right = glm::normalize(glm::cross(center.target - center.position, center.up));
    std::array<Camera, 2> cameras{center, center};
    std::array<View, 2> views{};
    for(size_t e = 0; e < 2; e++)
    {
        const glm::vec3 offset = right * (separation * (e == 0 ? -0.5f : 0.5f));
        cameras[e].position += offset;
        cameras[e].target += offset;
        views[e] = View{&cameras[e], eyeImages_[e], size, size};
    }
    drawViews(frame.commands, scene, views);
    drawMirror(frame);
}

SDL_GPUGraphicsPipeline* Renderer::eyeCopyPipeline(SDL_GPUTextureFormat format)
{
    Pipelines& p = *pipelines_;
    const auto found = p.eyeCopy.find(static_cast<int>(format));
    if(found != p.eyeCopy.end())
    {
        return found->second;
    }
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if(p.eyeCopyVert && p.eyeCopyFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = p.eyeCopyVert;
        info.fragment_shader = p.eyeCopyFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating the eye copy pipeline failed: {}", SDL_GetError());
        }
    }
    p.eyeCopy[static_cast<int>(format)] = pipeline; // not tried again if it failed
    return pipeline;
}

void Renderer::drawEyes(const Frame& frame, const Scene& scene,
                        const std::array<Camera, 2>& cameras, uint32_t width, uint32_t height)
{
    if(width == 0 || height == 0 || !ensureEyeImages(width, height))
    {
        return;
    }
    const std::array<View, 2> views{View{&cameras[0], eyeImages_[0], width, height},
                                    View{&cameras[1], eyeImages_[1], width, height}};
    drawViews(frame.commands, scene, views);
    drawMirror(frame);
}

void Renderer::copyEyes(const Frame& frame, const std::array<SDL_GPUTexture*, 2>& targets,
                        SDL_GPUTextureFormat format)
{
    SDL_GPUGraphicsPipeline* copy = eyeCopyPipeline(format);
    for(size_t e = 0; e < 2; e++)
    {
        if(!copy || !targets[e] || !eyeImages_[e])
        {
            continue;
        }
        SDL_GPUColorTargetInfo color = {};
        color.texture = targets[e];
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.commands, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, copy);
        const SDL_GPUTextureSamplerBinding eye = {eyeImages_[e], pipelines_->clampSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &eye, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
}

void Renderer::drawMirror(const Frame& frame)
{
    if(!frame.target || frame.width == 0 || frame.height == 0)
    {
        return;
    }
    if(!eyeImages_[0])
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = frame.target;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(frame.commands, &color, 1, nullptr));
        return;
    }
    // The middle of the eye, as much as has the window's shape.
    const float windowAspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const float eyeAspect = static_cast<float>(eyeWidth_) / static_cast<float>(eyeHeight_);
    uint32_t w = eyeWidth_;
    uint32_t h = eyeHeight_;
    if(windowAspect > eyeAspect)
    {
        h = std::max(1u, static_cast<uint32_t>(static_cast<float>(eyeWidth_) / windowAspect));
    }
    else
    {
        w = std::max(1u, static_cast<uint32_t>(static_cast<float>(eyeHeight_) * windowAspect));
    }
    SDL_GPUBlitInfo blit = {};
    blit.source.texture = eyeImages_[0];
    blit.source.x = (eyeWidth_ - w) / 2;
    blit.source.y = (eyeHeight_ - h) / 2;
    blit.source.w = w;
    blit.source.h = h;
    blit.destination.texture = frame.target;
    blit.destination.w = frame.width;
    blit.destination.h = frame.height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(frame.commands, &blit);
}

} // namespace viewer::render
