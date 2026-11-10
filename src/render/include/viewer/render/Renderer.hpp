#pragma once

#include "viewer/image/TextureData.hpp"
#include "viewer/render/GpuTexture.hpp"
#include "viewer/render/Mesh.hpp"
#include "viewer/render/Scene.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <optional>
#include <span>
#include <utility>
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

    // Draws the 3D scene (HDR, depth-tested) and tonemaps it into the frame's
    // target, replacing its contents. The UI is drawn on top afterwards.
    void drawScene(const Frame& frame, const Scene& scene);

    void endFrame();

    // Creates sampled textures and uploads every mip level, as one transfer
    // buffer and one copy pass. Output order matches input; a failed texture
    // comes back empty. Total upload size must stay under 4 GiB per call;
    // callers should batch to a per-frame budget anyway.
    // srgb: sample as sRGB (color data) instead of raw UNORM (normals, masks, UI).
    std::vector<GpuTexture> uploadTextures(std::span<const image::TextureData* const> textures,
                                           bool srgb);

    // Creates a 2D array texture, one layer per input (terrain uses 4). Every
    // layer must have the same size and number of mip levels.
    GpuTexture uploadTextureArray(std::span<const image::TextureData* const> layers, bool srgb);

    Mesh createMesh(std::span<const ObjectVertex> vertices, std::span<const uint32_t> indices);
    Mesh createMesh(std::span<const TerrainVertex> vertices, std::span<const uint32_t> indices);

    // Captures the scene (without UI) the next time drawScene runs; collect
    // it with takeScreenshot() after endFrame(). RGBA8, sRGB-encoded.
    void requestScreenshot() { screenshotRequested_ = true; }
    std::optional<image::MipLevel> takeScreenshot() { return std::exchange(screenshot_, {}); }

private:
    struct Pipelines;

    Renderer(SDL_Window* window, SDL_GPUDevice* device);
    bool createPipelines();
    bool createDefaultTextures();
    bool ensureTargets(uint32_t width, uint32_t height);
    Mesh createMesh(const void* vertices, size_t vertexBytes, std::span<const uint32_t> indices,
                    VertexLayout layout);
    void drawTonemap(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, float exposure,
                     bool clear);
    void finishScreenshot();

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUCommandBuffer* currentCommands_ = nullptr;

    std::unique_ptr<Pipelines> pipelines_;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* hdrTarget_ = nullptr;
    SDL_GPUTexture* depthTarget_ = nullptr;
    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;

    // Neutral stand-ins for missing material textures.
    GpuTexture white_;
    GpuTexture black_;
    GpuTexture flatNormal_;
    GpuTexture whiteLayers_;
    GpuTexture blackLayers_;
    GpuTexture flatNormalLayers_;

    bool screenshotRequested_ = false;
    SDL_GPUTexture* screenshotTexture_ = nullptr;
    SDL_GPUTransferBuffer* screenshotTransfer_ = nullptr;
    uint32_t screenshotWidth_ = 0;
    uint32_t screenshotHeight_ = 0;
    bool screenshotPending_ = false;
    std::optional<image::MipLevel> screenshot_;
};

} // namespace viewer::render
