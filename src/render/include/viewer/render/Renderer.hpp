#pragma once

#include "viewer/image/TextureData.hpp"
#include "viewer/render/GpuBuffer.hpp"
#include "viewer/render/GpuTexture.hpp"
#include "viewer/render/Mesh.hpp"
#include "viewer/render/MeshPool.hpp"
#include "viewer/render/Scene.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <memory>
#include <mutex>
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
    bool vsync = true;     // off: present as soon as a frame is done (for measuring)
};

// Owns the SDL GPU device and its binding to one window.
//
// Threading: swapchain acquisition and submission happen on the thread that
// created the window (the main thread). Other threads may record work on their
// own command buffers (e.g. texture uploads via copy passes).
struct Cascades;
struct FrameFS;
class ScenePass;

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
    // callers should batch anyway. Safe to call from any thread (calls are
    // serialized; each submits its own command buffer): textures are usable
    // in frames submitted after it returns.
    // srgb: sample as sRGB (color data) instead of raw UNORM (normals, masks, UI).
    std::vector<GpuTexture> uploadTextures(std::span<const image::TextureData* const> textures,
                                           bool srgb);

    // Creates a 2D array texture, one layer per input (terrain uses 4). Every
    // layer must have the same size and number of mip levels.
    GpuTexture uploadTextureArray(std::span<const image::TextureData* const> layers, bool srgb);

    // Replaces the terrain blend ramp (the generated stand-in) with this one:
    // u = position between two layers, v = blend noise; red channel used.
    void setTerrainBlendRamp(const image::TextureData& ramp);

    Mesh createMesh(std::span<const ObjectVertex> vertices, std::span<const uint32_t> indices);
    Mesh createMesh(std::span<const TerrainVertex> vertices, std::span<const uint32_t> indices);
    Mesh createMesh(std::span<const SkinnedVertex> vertices, std::span<const uint32_t> indices);

    // Captures the scene (without UI) the next time drawScene runs; collect
    // it with takeScreenshot() after endFrame(). RGBA8, sRGB-encoded.
    void requestScreenshot() { screenshotRequested_ = true; }

    // Draws into each shadow cascade last frame (for the stats).
    const std::array<uint32_t, 4>& shadowDraws() const { return shadowDrawCounts_; }
    // Local lights used last frame.
    uint32_t lightsUsed() const { return lightsUsed_; }
    // Bytes in the mesh pool's GPU buffers (vertices and indices).
    uint64_t meshPoolBytes() const;
    std::optional<image::MipLevel> takeScreenshot() { return std::exchange(screenshot_, {}); }

    // The GPU pipelines and samplers (internal: RendererPipelines.hpp).
    struct Pipelines;

private:
    Renderer(SDL_Window* window, SDL_GPUDevice* device);
    bool createPipelines();
    bool createDefaultTextures();
    bool ensureTargets(uint32_t width, uint32_t height);
    Mesh createMesh(const void* vertices, size_t vertexBytes, std::span<const uint32_t> indices,
                    VertexLayout layout);
    void drawTonemap(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, const Lighting& light,
                     bool clear);
    // Measures the frame's luminance and moves the dynamic exposure towards
    // what it calls for (into exposureMaps_[exposureIndex_]).
    void updateExposure(SDL_GPUCommandBuffer* commands, const Lighting& light);
    void finishScreenshot();
    // A GPU buffer refilled every frame through its own upload buffer.
    struct FrameBuffer
    {
        GpuBuffer buffer;
        SDL_GPUTransferBuffer* transfer = nullptr; // same size as buffer
    };
    // Makes `b` at least `bytes` large (contents lost when it grows).
    bool reserve(FrameBuffer& b, uint32_t bytes, SDL_GPUBufferUsageFlags usage);
    // Uploads every object draw's data (by index into scene.objects) and the
    // skinned draws' joint matrices; false if there's nothing to draw or
    // the buffers couldn't be made.
    bool uploadDraws(SDL_GPUCommandBuffer* commands, const Scene& scene);
    // Picks the lights to use (those reaching the view, nearest first, at
    // most kMaxLights) and uploads them; returns how many.
    uint32_t uploadLights(SDL_GPUCommandBuffer* commands, const Scene& scene);

public:
    static constexpr uint32_t kMaxLights = 64;

private:
    // The sky (behind everything) and the terrain, as seen with `view` and
    // `projection` (the view's, or a reflection probe face's).
    void drawSky(ScenePass& pass, const Atmosphere& atmo, const glm::mat4& view,
                 const glm::mat4& projection);
    void drawTerrain(ScenePass& pass, const Scene& scene);

    // Reflection probes (RendererProbes.cpp), as the official viewer's
    // default probe: a cube of the sky and terrain around the camera,
    // captured a face a frame every Reflections::updateSeconds, then
    // filtered into radiance (blurrier mip levels for rougher surfaces) and
    // irradiance (ambient light). Cubes are kept mirrored in y (probes.glsl).
    bool createProbes();
    // Captures the next face, or filters a finished capture.
    void updateProbes(SDL_GPUCommandBuffer* commands, const Scene& scene, const FrameFS& frameFs);
    void filterProbe(SDL_GPUCommandBuffer* commands);
    // The probe textures, as samplers 'radiance, irradiance, BRDF table'.
    std::array<SDL_GPUTextureSamplerBinding, 3> probeSamplers() const;

    // Renders the shadow casters into each cascade's layer of the shadow map.
    void drawShadows(SDL_GPUCommandBuffer* commands, const Scene& scene, const Cascades& cascades,
                     bool drawsReady);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUCommandBuffer* currentCommands_ = nullptr;

    std::unique_ptr<Pipelines> pipelines_;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* hdrTarget_ = nullptr;
    SDL_GPUTexture* depthTarget_ = nullptr;
    // Dynamic exposure (as the official viewer's): the scene's luminance,
    // mipmapped down to its average, and the exposure (1x1), last frame's
    // and this one's.
    static constexpr uint32_t kLuminanceSize = 256;
    static constexpr uint32_t kLuminanceLevels = 9; // 256 down to 1
    SDL_GPUTexture* luminance_ = nullptr;
    std::array<SDL_GPUTexture*, 2> exposureMaps_{};
    uint32_t exposureIndex_ = 0;
    bool exposureHistory_ = false; // exposureMaps_ hold a valid exposure
    uint64_t lastExposureTicks_ = 0;
    // Sun shadows: one layer per cascade.
    static constexpr uint32_t kShadowCascades = 4;
    static constexpr uint32_t kShadowMapSize = 2048;
    SDL_GPUTextureFormat shadowFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* shadowMap_ = nullptr;
    std::vector<glm::vec4> casterSpheres_; // xyz center, w radius, per scene object
    std::array<uint32_t, 4> shadowDrawCounts_{};
    uint32_t lightsUsed_ = 0;
    // Projector textures, each copied (scaled, into every mip level) into a
    // layer of one array, so every surface shader samples them through one
    // binding. A layer keeps its texture until another needs the room.
    static constexpr uint32_t kProjectorLayers = 8;
    static constexpr uint32_t kProjectorSize = 512;
    static constexpr uint32_t kProjectorLevels = 10; // 512 down to 1
    SDL_GPUTexture* projectors_ = nullptr;
    struct ProjectorLayer
    {
        SDL_GPUTexture* source = nullptr;
        uint32_t width = 0;
        uint64_t usedIn = 0; // the light upload that last used it
    };
    std::array<ProjectorLayer, kProjectorLayers> projectorLayers_{};
    uint64_t lightUploads_ = 0;
    // The layer holding `texture` (copied there if need be); -1 if every
    // layer is taken this frame.
    int projectorLayer(SDL_GPUCommandBuffer* commands, const GpuTexture& texture);
    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;

    // Reflection probes: the radiance and irradiance cubes (cube arrays, one
    // cube per probe, so far just the default one), the cube being captured
    // (mipmapped, for filtering) with its depth, and the split-sum table.
    static constexpr uint32_t kProbeSize = 128; // RenderReflectionProbeResolution
    static constexpr uint32_t kProbeLevels = 7; // 128 down to 2 (max_probe_lod 6)
    static constexpr uint32_t kIrradianceSize = 16;
    static constexpr uint32_t kBrdfTableSize = 128;
    static constexpr uint32_t kProbeCount = 1;
    SDL_GPUTexture* probeRadiance_ = nullptr;
    SDL_GPUTexture* probeIrradiance_ = nullptr;
    SDL_GPUTexture* probeCapture_ = nullptr;
    SDL_GPUTexture* probeDepth_ = nullptr;
    SDL_GPUTexture* brdfTable_ = nullptr;
    struct ProbeUpdate
    {
        int face = -1;          // the next face to capture; -1: none under way
        glm::vec3 origin{0.0f}; // where it's captured from
        uint64_t startedTicks = 0;
        bool ready = false; // the cubes hold a filtered capture
    };
    ProbeUpdate probe_;

    // Neutral stand-ins for missing material textures.
    GpuTexture white_;
    GpuTexture black_;
    GpuTexture flatNormal_;
    // A base color still loading: the official viewer's default image
    // (llviewertexture.cpp, flat 0x7f gray), not white, which lights wash out.
    GpuTexture loading_;
    GpuTexture whiteLayers_;
    GpuTexture blackLayers_;
    GpuTexture flatNormalLayers_;
    GpuTexture terrainBlendRamp_;

    std::unique_ptr<MeshPool> meshPool_;
    std::vector<std::pair<uint64_t, const ObjectDraw*>> keyedDraws_; // drawScene's, kept
    FrameBuffer drawBuffer_;     // this frame's DrawData, one per object draw
    FrameBuffer jointBuffer_;    // this frame's joint matrices
    FrameBuffer instanceBuffer_; // 0, 1, 2, ...: each draw's index, per instance
    FrameBuffer lightBuffer_;    // this frame's local lights (lights.glsl)
    std::mutex uploadMutex_;     // uploadTextures runs on workers too
    SDL_GPUTransferBuffer* textureStaging_ = nullptr; // for uploadTextures batches

    bool screenshotRequested_ = false;
    SDL_GPUTexture* screenshotTexture_ = nullptr;
    SDL_GPUTransferBuffer* screenshotTransfer_ = nullptr;
    uint32_t screenshotWidth_ = 0;
    uint32_t screenshotHeight_ = 0;
    bool screenshotPending_ = false;
    std::optional<image::MipLevel> screenshot_;
};

} // namespace viewer::render
