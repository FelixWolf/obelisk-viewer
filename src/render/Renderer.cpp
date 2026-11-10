#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <cassert>

namespace viewer::render {

std::unique_ptr<Renderer> Renderer::create(SDL_Window* window, const RendererOptions& options)
{
    const SDL_GPUShaderFormat formats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;

    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, options.gpuDebug, nullptr);
    if (!device) {
        log::error("SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return nullptr;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        log::error("SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        return nullptr;
    }

    SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);

    log::info("GPU device created (driver: {})", SDL_GetGPUDeviceDriver(device));
    return std::unique_ptr<Renderer>(new Renderer(window, device));
}

Renderer::Renderer(SDL_Window* window, SDL_GPUDevice* device)
    : window_(window), device_(device)
{
}

Renderer::~Renderer()
{
    SDL_WaitForGPUIdle(device_);
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
    if (!currentCommands_) {
        log::error("SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return std::nullopt;
    }

    Frame frame;
    frame.commands = currentCommands_;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(currentCommands_, window_, &frame.target,
                                               &frame.width, &frame.height)) {
        log::error("SDL_WaitAndAcquireGPUSwapchainTexture failed: {}", SDL_GetError());
        return std::nullopt;
    }
    if (!frame.target) {
        return std::nullopt; // minimized or too many frames in flight
    }
    return frame;
}

void Renderer::drawScene(const Frame& frame)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = frame.target;
    color.clear_color = SDL_FColor{0.08f, 0.10f, 0.14f, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.commands, &color, 1, nullptr);
    SDL_EndGPURenderPass(pass);
}

void Renderer::endFrame()
{
    if (!currentCommands_) {
        return;
    }
    if (!SDL_SubmitGPUCommandBuffer(currentCommands_)) {
        log::error("SDL_SubmitGPUCommandBuffer failed: {}", SDL_GetError());
    }
    currentCommands_ = nullptr;
}

} // namespace viewer::render
