// Occlusion culling: after the scene pass, the view's depth made small
// (occlusion_reduce.frag: the farthest depth in each block, kOcclusionWidth
// wide) and copied into one of a few download buffers; once that frame's
// fence says the GPU is done (pollFrames), it becomes the OcclusionMap the
// next scenes are culled with. Nothing waits for the GPU.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <cmath>
#include <cstring>

namespace viewer::render
{

namespace
{

struct ReduceFS
{
    glm::vec4 size; // xy the target's
};

} // namespace

void Renderer::drawOcclusion(SDL_GPUCommandBuffer* commands, const Scene& scene,
                             const glm::mat4& viewProjection)
{
    occlusionSlot_ = -1;
    if(!depthTarget_ || targetWidth_ == 0 || targetHeight_ == 0)
    {
        return;
    }
    int slot = -1;
    for(int i = 0; i < kOcclusionSlots; i++)
    {
        if(!occlusionSlots_[static_cast<size_t>(i)].busy)
        {
            slot = i;
            break;
        }
    }
    if(slot < 0)
    {
        return; // all on their way back
    }
    const uint32_t width = kOcclusionWidth;
    const auto height = std::max<uint32_t>(
        1, static_cast<uint32_t>(
               std::lround(static_cast<double>(kOcclusionWidth) * targetHeight_ / targetWidth_)));
    if(!occlusionTexture_ || occlusionWidth_ != width || occlusionHeight_ != height)
    {
        if(occlusionTexture_)
        {
            SDL_ReleaseGPUTexture(device_, occlusionTexture_);
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kOcclusionFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        occlusionTexture_ = SDL_CreateGPUTexture(device_, &info);
        occlusionWidth_ = width;
        occlusionHeight_ = height;
    }
    OcclusionSlot& s = occlusionSlots_[static_cast<size_t>(slot)];
    const uint32_t bytes = width * height * 4;
    if(!s.transfer || s.capacity < bytes)
    {
        if(s.transfer)
        {
            SDL_ReleaseGPUTransferBuffer(device_, s.transfer);
        }
        SDL_GPUTransferBufferCreateInfo info = {};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        s.transfer = SDL_CreateGPUTransferBuffer(device_, &info);
        s.capacity = s.transfer ? bytes : 0;
    }
    if(!occlusionTexture_ || !s.transfer)
    {
        log::error("creating the occlusion map failed: {}", SDL_GetError());
        return;
    }

    SDL_GPUColorTargetInfo color = {};
    color.texture = occlusionTexture_;
    color.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->occlusionReduce);
    const SDL_GPUTextureSamplerBinding depth = {depthTarget_, pipelines_->clampSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &depth, 1);
    pushFragmentUniforms(
        commands, 0,
        ReduceFS{glm::vec4(static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f)});
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion source = {};
    source.texture = occlusionTexture_;
    source.w = width;
    source.h = height;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination = {};
    destination.transfer_buffer = s.transfer;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);

    s.busy = true;
    s.width = width;
    s.height = height;
    s.viewProjection = viewProjection;
    s.origin = scene.worldOrigin;
    occlusionSlot_ = slot;
}

void Renderer::readOcclusion(int slot)
{
    OcclusionSlot& s = occlusionSlots_[static_cast<size_t>(slot)];
    s.busy = false;
    const auto* data =
        static_cast<const float*>(SDL_MapGPUTransferBuffer(device_, s.transfer, false));
    if(!data)
    {
        return;
    }
    std::vector<float> depth(static_cast<size_t>(s.width) * s.height);
    std::memcpy(depth.data(), data, depth.size() * sizeof(float));
    SDL_UnmapGPUTransferBuffer(device_, s.transfer);
    occlusion_ = std::make_shared<const OcclusionMap>(s.width, s.height, std::move(depth),
                                                      s.viewProjection, s.origin);
}

void Renderer::pollFrames()
{
    while(!inFlight_.empty() && SDL_QueryGPUFence(device_, inFlight_.front().fence))
    {
        const InFlight f = inFlight_.front();
        inFlight_.pop_front();
        SDL_ReleaseGPUFence(device_, f.fence);
        if(f.pick)
        {
            readPick();
        }
        if(f.occlusionSlot >= 0)
        {
            readOcclusion(f.occlusionSlot);
        }
    }
}

} // namespace viewer::render
