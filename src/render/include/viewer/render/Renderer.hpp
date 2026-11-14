#pragma once

#include "viewer/image/TextureData.hpp"
#include "viewer/render/GpuBuffer.hpp"
#include "viewer/render/GpuTexture.hpp"
#include "viewer/render/Mesh.hpp"
#include "viewer/render/MeshPool.hpp"
#include "viewer/render/OcclusionMap.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/text/Font.hpp"
#include "viewer/text/GlyphAtlas.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <deque>
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
struct FrameVS;
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
    void drawScene(const Frame& frame, const Scene& scene)
    {
        drawScene(frame, scene, nullptr, true);
    }
    // Stereo, as a headset will need (and, until there is one, to see what
    // that costs: --stereo-benchmark): the scene once per eye, each `size`
    // square, from the scene's camera moved sideways by half `separation`
    // (meters) each way, into the eye images; then the left eye shown in
    // the frame. Each eye is a whole scene render for now.
    void drawStereo(const Frame& frame, const Scene& scene, uint32_t size, float separation);
    // The eye images drawStereo last drew (left, right; null before).
    std::array<SDL_GPUTexture*, 2> eyeImages() const { return eyeImages_; }

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
    // What the next frame's reflection probe capture sees (its face's
    // view-projection), if one is under way: objects in it should be in the
    // scene even out of view (Reflections::probeFace).
    std::optional<Frustum> nextProbeFace() const;
    // The HUD image last drawn (premultiplied, sRGB-encoded values; null
    // before any HUD): for showing it elsewhere, such as in a headset.
    SDL_GPUTexture* hudImage() const { return hudImage_; }
    // HUD space to the HUD image's clip space (render::Hud).
    static glm::mat4 hudViewProjection(const Hud& hud);
    // The answer to the latest Scene::pick that has come back from the GPU
    // (once; nothing while one is on its way).
    std::optional<PickResult> takePick()
    {
        pollFrames();
        return std::exchange(pick_, {});
    }
    // What a recent frame hid (occlusion culling: Scene::occlusion); null
    // until one has come back from the GPU.
    std::shared_ptr<const OcclusionMap> occlusion()
    {
        pollFrames();
        return occlusion_;
    }
    // Local probes surfaces used last frame, and cubes holding a capture.
    uint32_t probesUsed() const { return probesShaded_; }
    uint32_t probesCaptured() const;
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
    // The scene as seen by `camera` (the scene's if null); `primary`: the
    // frame's first view, which also does what's done once a frame (the
    // pick, the occlusion map).
    void drawScene(const Frame& frame, const Scene& scene, const Camera* camera, bool primary);
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
    // Object draws, in this order (indices into scene.objects as instances).
    void drawObjects(ScenePass& pass, const Scene& scene, std::span<const ObjectDraw* const> order);

    // Reflection probes (RendererProbes.cpp), as the official viewer's: the
    // default probe (a cube of the sky and terrain above the camera, every
    // Reflections::updateSeconds) in cube 0, and the local probes nearest
    // the camera (Scene::probes) in the others, captured one face a frame,
    // then filtered into radiance (blurrier mip levels for rougher
    // surfaces) and irradiance (ambient light). Cubes are kept mirrored in y
    // (probes.glsl).
    bool createProbes();
    // Gives the probes cubes, uploads those surfaces use (and says so in
    // frameFs), and captures the next face of the one being updated.
    void updateProbes(SDL_GPUCommandBuffer* commands, const Scene& scene, FrameFS& frameFs,
                      bool drawsReady);
    void captureProbeFace(SDL_GPUCommandBuffer* commands, const Scene& scene,
                          const FrameFS& frameFs, bool drawsReady);
    void filterProbe(SDL_GPUCommandBuffer* commands, uint32_t cube);
    // The probe textures, as samplers 'radiance, irradiance, BRDF table'.
    std::array<SDL_GPUTextureSamplerBinding, 3> probeSamplers() const;

    // HUD attachments (RendererHud.cpp): into the HUD image, `width` x
    // `height` (false if there are none); that image over `target`, or on a
    // panel in the scene.
    bool drawHud(SDL_GPUCommandBuffer* commands, const Scene& scene, uint32_t width,
                 uint32_t height, bool drawsReady);
    void compositeHud(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target);
    void drawHudPanel(ScenePass& pass, const Hud& hud, const glm::mat4& viewProjection);

    // Picking (RendererPick.cpp): the pick pixels, read back once the
    // frame's fence says they're done.
    void drawPick(SDL_GPUCommandBuffer* commands, const Scene& scene, bool drawsReady);
    void readPick();
    // Frames whose results come back once the GPU is done with them (a
    // pick, the occlusion map), oldest first; pollFrames reads those done.
    struct InFlight
    {
        SDL_GPUFence* fence = nullptr;
        bool pick = false;
        int occlusionSlot = -1;
    };
    std::deque<InFlight> inFlight_;
    void pollFrames();
    // Occlusion culling (RendererOcclusion.cpp): the view's depth made small
    // and read back into one of a few buffers, a frame or two later.
    void drawOcclusion(SDL_GPUCommandBuffer* commands, const Scene& scene,
                       const glm::mat4& viewProjection);
    void readOcclusion(int slot);
    // The outline around Scene::highlight, over `target`.
    void drawOutline(SDL_GPUCommandBuffer* commands, const Scene& scene, SDL_GPUTexture* target,
                     uint32_t width, uint32_t height, const FrameVS& frameVs);

    // Labels (RendererText.cpp): glyph quads for this frame's labels (and the
    // glyphs new to the atlas) up to the GPU, then drawn over `target`
    // (depth tested against the view), or into the HUD image.
    void uploadLabels(SDL_GPUCommandBuffer* commands, const Scene& scene,
                      const glm::mat4& viewProjection, uint32_t width, uint32_t height);
    void drawLabels(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, bool hud);

    // Glow (RendererGlow.cpp), as the official viewer's: how much each pixel
    // glows (the glowing faces, depth tested against the view), the view as
    // displayed times that into a small image, blurred; the tonemap pass
    // adds the result (glowResult_, null if nothing glows).
    void drawGlow(SDL_GPUCommandBuffer* commands, const Scene& scene, const Lighting& light,
                  uint32_t width, uint32_t height, const FrameVS& frameVs, bool drawsReady);

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

    std::array<SDL_GPUTexture*, 4> pickTargets_{};
    SDL_GPUTexture* pickDepth_ = nullptr;
    SDL_GPUTransferBuffer* pickTransfer_ = nullptr;
    bool pickInFlight_ = false;  // a submitted frame holds a pick, until it's done
    bool pickSubmitted_ = false; // this frame draws a pick
    static constexpr uint32_t kOcclusionWidth = 256;
    static constexpr int kOcclusionSlots = 3;
    struct OcclusionSlot
    {
        SDL_GPUTransferBuffer* transfer = nullptr;
        uint32_t capacity = 0; // bytes
        uint32_t width = 0;
        uint32_t height = 0;
        glm::mat4 viewProjection{1.0f};
        glm::dvec2 origin{0.0};
        bool busy = false;
    };
    std::array<OcclusionSlot, kOcclusionSlots> occlusionSlots_{};
    SDL_GPUTexture* occlusionTexture_ = nullptr;
    uint32_t occlusionWidth_ = 0;
    uint32_t occlusionHeight_ = 0;
    int occlusionSlot_ = -1; // this frame's
    std::shared_ptr<const OcclusionMap> occlusion_;
    uint64_t pickRequest_ = 0;
    std::optional<PickResult> pick_;
    SDL_GPUTexture* glowMask_ = nullptr;
    uint32_t glowMaskWidth_ = 0;
    uint32_t glowMaskHeight_ = 0;
    std::array<SDL_GPUTexture*, 3> glowImages_{};
    SDL_GPUTexture* glowResult_ = nullptr; // this frame's, if anything glows
    SDL_GPUTexture* outlineMask_ = nullptr;
    uint32_t outlineWidth_ = 0;
    uint32_t outlineHeight_ = 0;

    // Labels: the font, its glyphs (on the CPU, and as a texture), and this
    // frame's glyph quads, those in the scene first, then the HUD's.
    std::shared_ptr<const text::Font> font_;
    std::unique_ptr<text::GlyphAtlas> glyphs_;
    SDL_GPUTexture* glyphTexture_ = nullptr;
    SDL_GPUTransferBuffer* glyphTransfer_ = nullptr;
    FrameBuffer labelBuffer_;
    uint32_t labelVertices_ = 0;
    uint32_t hudLabelVertices_ = 0;

    std::array<SDL_GPUTexture*, 2> eyeImages_{};
    uint32_t eyeSize_ = 0;

    SDL_GPUTexture* hudImage_ = nullptr;
    SDL_GPUTexture* hudDepth_ = nullptr;
    uint32_t hudWidth_ = 0;
    uint32_t hudHeight_ = 0;

    // Reflection probes: the radiance and irradiance cubes (cube arrays, one
    // cube per probe, so far just the default one), the cube being captured
    // (mipmapped, for filtering) with its depth, and the split-sum table.
    static constexpr uint32_t kProbeSize = 128; // RenderReflectionProbeResolution
    static constexpr uint32_t kProbeLevels = 7; // 128 down to 2 (max_probe_lod 6)
    static constexpr uint32_t kIrradianceSize = 16;
    static constexpr uint32_t kBrdfTableSize = 128;
    static constexpr uint32_t kProbeCount = 64; // cubes: the default probe and 63 local
    static constexpr uint32_t kProbesShaded =
        32; // local probes surfaces look at (REF_SAMPLE_COUNT)
    SDL_GPUTexture* probeRadiance_ = nullptr;
    SDL_GPUTexture* probeIrradiance_ = nullptr;
    SDL_GPUTexture* probeCapture_ = nullptr;
    SDL_GPUTexture* probeDepth_ = nullptr;
    SDL_GPUTexture* brdfTable_ = nullptr;
    // What each cube holds.
    struct ProbeCube
    {
        uint64_t id = 0; // the local probe's (cube 0: the default probe)
        bool used = false;
        bool complete = false;       // holds a filtered capture
        uint64_t completedTicks = 0; // its first: it fades in from then
        uint64_t updatedTicks = 0;   // its latest
    };
    std::array<ProbeCube, kProbeCount> probeCubes_{};
    // The capture under way: a face a frame into probeCapture_.
    struct ProbeCapture
    {
        int face = -1; // the next one; -1: none under way
        uint32_t cube = 0;
        glm::vec3 origin{0.0f};
        float nearClip = 0.1f;
        float farClip = 64.0f;
        bool objects = false; // local probes see objects, the default one doesn't
    };
    ProbeCapture capture_;
    FrameBuffer probeBuffer_; // the local probes surfaces use this frame
    uint32_t probesShaded_ = 0;

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
