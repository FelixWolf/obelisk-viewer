#include "viewer/render/Renderer.hpp"

#include "RendererPipelines.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <cassert>
#include <cstring>

namespace viewer::render
{

std::unique_ptr<Renderer> Renderer::create(SDL_Window* window, const RendererOptions& options)
{
    // Shaders are built as SPIR-V only for now, which selects Vulkan (Linux,
    // Windows). Metal (MSL) and D3D12 (DXIL) need converted shaders first.
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;

    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, options.gpuDebug, nullptr);
    if(!device)
    {
        log::error("SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return nullptr;
    }

    if(!SDL_ClaimWindowForGPUDevice(device, window))
    {
        log::error("SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        return nullptr;
    }

    SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);

    log::info("GPU device created (driver: {})", SDL_GetGPUDeviceDriver(device));
    std::unique_ptr<Renderer> renderer(new Renderer(window, device));
    if(!renderer->createPipelines() || !renderer->createDefaultTextures())
    {
        return nullptr;
    }
    return renderer;
}

Renderer::Renderer(SDL_Window* window, SDL_GPUDevice* device) : window_(window), device_(device) {}

Renderer::~Renderer()
{
    SDL_WaitForGPUIdle(device_);
    // Everything created on the device goes before the device itself.
    pipelines_.reset();
    white_.reset();
    black_.reset();
    flatNormal_.reset();
    whiteLayers_.reset();
    blackLayers_.reset();
    flatNormalLayers_.reset();
    for(SDL_GPUTexture* t : {hdrTarget_, depthTarget_, screenshotTexture_})
    {
        if(t)
        {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    if(screenshotTransfer_)
    {
        SDL_ReleaseGPUTransferBuffer(device_, screenshotTransfer_);
    }
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

SDL_GPUTextureFormat Renderer::swapchainFormat() const
{
    return SDL_GetGPUSwapchainTextureFormat(device_, window_);
}

const char* Renderer::driverName() const
{
    return SDL_GetGPUDeviceDriver(device_);
}

std::optional<Frame> Renderer::beginFrame()
{
    assert(!currentCommands_ && "beginFrame called twice without endFrame");

    currentCommands_ = SDL_AcquireGPUCommandBuffer(device_);
    if(!currentCommands_)
    {
        log::error("SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return std::nullopt;
    }

    Frame frame;
    frame.commands = currentCommands_;
    if(!SDL_WaitAndAcquireGPUSwapchainTexture(currentCommands_, window_, &frame.target,
                                              &frame.width, &frame.height))
    {
        log::error("SDL_WaitAndAcquireGPUSwapchainTexture failed: {}", SDL_GetError());
        return std::nullopt;
    }
    if(!frame.target)
    {
        return std::nullopt; // minimized or too many frames in flight
    }
    return frame;
}

std::vector<GpuTexture>
Renderer::uploadTextures(std::span<const image::TextureData* const> textures, bool srgb)
{
    std::vector<GpuTexture> out(textures.size());

    uint64_t totalBytes = 0;
    for(const image::TextureData* t : textures)
    {
        for(const auto& level : t->levels)
        {
            totalBytes += level.byteSize();
        }
    }
    if(totalBytes == 0 || totalBytes > UINT32_MAX)
    {
        if(totalBytes > UINT32_MAX)
        {
            log::error("uploadTextures: batch of {} bytes exceeds 4 GiB", totalBytes);
        }
        return out;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(totalBytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if(!transfer)
    {
        log::error("SDL_CreateGPUTransferBuffer failed: {}", SDL_GetError());
        return out;
    }

    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if(!mapped)
    {
        log::error("SDL_MapGPUTransferBuffer failed: {}", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return out;
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if(!commands)
    {
        log::error("SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return out;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);

    Uint32 offset = 0;
    for(size_t i = 0; i < textures.size(); i++)
    {
        const image::TextureData& data = *textures[i];
        if(data.levels.empty())
        {
            continue;
        }

        SDL_GPUTextureCreateInfo createInfo = {};
        createInfo.type = SDL_GPU_TEXTURETYPE_2D;
        createInfo.format =
            srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        createInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        createInfo.width = data.width();
        createInfo.height = data.height();
        createInfo.layer_count_or_depth = 1;
        createInfo.num_levels = static_cast<Uint32>(data.levels.size());
        createInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &createInfo);
        if(!texture)
        {
            log::error("SDL_CreateGPUTexture failed: {}", SDL_GetError());
            for(const auto& level : data.levels)
            {
                offset += static_cast<Uint32>(level.byteSize());
            }
            continue;
        }

        for(size_t level = 0; level < data.levels.size(); level++)
        {
            const image::MipLevel& mip = data.levels[level];
            std::memcpy(mapped + offset, mip.pixels.data(), mip.byteSize());

            SDL_GPUTextureTransferInfo source = {};
            source.transfer_buffer = transfer;
            source.offset = offset;
            source.pixels_per_row = mip.width;
            source.rows_per_layer = mip.height;

            SDL_GPUTextureRegion destination = {};
            destination.texture = texture;
            destination.mip_level = static_cast<Uint32>(level);
            destination.w = mip.width;
            destination.h = mip.height;
            destination.d = 1;

            SDL_UploadToGPUTexture(copy, &source, &destination, false);
            offset += static_cast<Uint32>(mip.byteSize());
        }
        out[i] = GpuTexture(device_, texture, data.width(), data.height(),
                            static_cast<uint32_t>(data.levels.size()));
    }

    SDL_EndGPUCopyPass(copy);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    if(!SDL_SubmitGPUCommandBuffer(commands))
    {
        log::error("SDL_SubmitGPUCommandBuffer (upload) failed: {}", SDL_GetError());
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer); // deferred until the upload completes
    return out;
}

void Renderer::endFrame()
{
    if(!currentCommands_)
    {
        return;
    }
    if(screenshotPending_)
    {
        // Rare (screenshots only), so simply wait for this frame to finish.
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(currentCommands_);
        currentCommands_ = nullptr;
        if(fence)
        {
            SDL_WaitForGPUFences(device_, true, &fence, 1);
            SDL_ReleaseGPUFence(device_, fence);
            finishScreenshot();
        }
        screenshotPending_ = false;
        return;
    }
    if(!SDL_SubmitGPUCommandBuffer(currentCommands_))
    {
        log::error("SDL_SubmitGPUCommandBuffer failed: {}", SDL_GetError());
    }
    currentCommands_ = nullptr;
}

} // namespace viewer::render
