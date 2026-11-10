#pragma once

#include "viewer/image/TextureData.hpp"
#include "viewer/render/GpuTexture.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace viewer::render
{

// One frame's worth of GPU recording state. Passed to anything that wants to
// draw into the frame (scene, UI, later: per-eye XR views).
struct Frame
{
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUTexture* target = nullptr; // swapchain image
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RendererOptions
{
    bool gpuDebug = false; // enable backend validation layers
};

// Owns the SDL GPU device and its binding to one window.
//
// Threading: swapchain acquisition and submission happen on the thread that
// created the window (the main thread). Other threads may record work on their
// own command buffers (e.g. texture uploads via copy passes).
class Renderer
{
public:
    static std::unique_ptr<Renderer> create(SDL_Window* window, const RendererOptions& options);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    SDL_GPUDevice* device() const { return device_; }
    SDL_GPUTextureFormat swapchainFormat() const;
    const char* driverName() const;

    // Returns nullopt if no swapchain image is available (e.g. minimized).
    // The frame must always be finished with endFrame(), even then.
    std::optional<Frame> beginFrame();

    // Placeholder for the world renderer: clears the target.
    void drawScene(const Frame& frame);

    void endFrame();

    // Creates sampled textures and uploads every mip level, as one transfer
    // buffer and one copy pass. Output order matches input; a failed texture
    // comes back empty. Total upload size must stay under 4 GiB per call;
    // callers should batch to a per-frame budget anyway.
    // srgb: sample as sRGB (color data) instead of raw UNORM (normals, masks, UI).
    std::vector<GpuTexture> uploadTextures(std::span<const image::TextureData* const> textures,
                                           bool srgb);

private:
    Renderer(SDL_Window* window, SDL_GPUDevice* device);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUCommandBuffer* currentCommands_ = nullptr;
};

} // namespace viewer::render
