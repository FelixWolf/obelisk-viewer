#pragma once

#include "viewer/image/TextureData.hpp"
#include "viewer/render/Clusters.hpp"
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
#include <functional>
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
    // More for the GPU device's creation properties (a headset's: the device
    // then drives it). If the device can't be made with them, it's made
    // without (extendedDevice() says which).
    std::function<void(SDL_PropertiesID)> deviceProperties;
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
struct ScenePipelines;

class Renderer
{
public:
    static std::unique_ptr<Renderer> create(SDL_Window* window, const RendererOptions& options);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    SDL_GPUDevice* device() const { return device_; }
    // Made with RendererOptions::deviceProperties.
    bool extendedDevice() const { return extendedDevice_; }
    SDL_GPUTextureFormat swapchainFormat() const;
    const char* driverName() const;

    // Multisampling: the samples a pixel the scene is drawn with (1, 2 or
    // 4; fewer if the GPU doesn't do as many). Between frames only.
    void setSamples(uint32_t samples);

    // Returns nullopt if no swapchain image is available (e.g. minimized).
    // The frame must always be finished with endFrame(), even then.
    // Without waiting for the window (a headset sets the pace): the frame
    // comes back anyway, its target null if the window has no image free,
    // or (`window` false) without asking it for one: the "no wait" acquire
    // can still block in the driver while the window isn't shown.
    std::optional<Frame> beginFrame(bool waitForWindow = true, bool window = true);

    // Draws the 3D scene (HDR, depth-tested) and tonemaps it into the frame's
    // target, replacing its contents. The UI is drawn on top afterwards.
    // A view of the scene: its camera, and the image it's drawn into (all a
    // frame's views the same size).
    struct View
    {
        const Camera* camera = nullptr;
        SDL_GPUTexture* target = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    void drawScene(const Frame& frame, const Scene& scene)
    {
        const View view{&scene.camera, frame.target, frame.width, frame.height};
        drawViews(frame.commands, scene, {&view, 1});
    }
    // Stereo, as a headset will need (and, until there is one, to see what
    // that costs: --stereo-benchmark): the scene once per eye, each `size`
    // square, from the scene's camera moved sideways by half `separation`
    // (meters) each way, into the eye images; then the left eye shown in
    // the frame. What the eyes share is done once (drawViews).
    void drawStereo(const Frame& frame, const Scene& scene, uint32_t size, float separation);
    // A headset's eyes (xr::Headset): the scene from each eye's camera into
    // the eye images (`width` x `height`), the left one also filling the
    // frame's target, if it has one. Then (copyEyes) into the headset's
    // images (`targets`, of `format`), as linear values: OpenXR takes 8-bit
    // UNORM images' values as linear too, and sRGB ones are encoded on the
    // way in. Apart, so the headset's images are needed only at the end.
    void drawEyes(const Frame& frame, const Scene& scene, const std::array<Camera, 2>& cameras,
                  uint32_t width, uint32_t height);
    void copyEyes(const Frame& frame, const std::array<SDL_GPUTexture*, 2>& targets,
                  SDL_GPUTextureFormat format);
    // The left eye image last drawn, filling the frame's target (cropped to
    // its shape); black before there is one.
    void drawMirror(const Frame& frame);
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
    // This frame's screenshot image while it's being made (null: none):
    // what's drawn over the window is drawn over it too, before endFrame
    // reads it back.
    SDL_GPUTexture* screenshotImage() const
    {
        return screenshotDrawn_ ? screenshotTexture_ : nullptr;
    }

    // The GPU pipelines and samplers (internal: RendererPipelines.hpp).
    struct Pipelines;

private:
    Renderer(SDL_Window* window, SDL_GPUDevice* device, bool extendedDevice);
    bool createPipelines();
    bool createScenePipelines(ScenePipelines& set, uint32_t samples);
    ScenePipelines& scenePipelines();
    // The multisampled depth into depthTarget_, if multisampling.
    void resolveDepth(SDL_GPUCommandBuffer* commands);
    bool createDefaultTextures();
    bool ensureTargets(uint32_t width, uint32_t height);
    // The scene in each view. What doesn't depend on the view is done once
    // (uploads, shadows, reflection probes, the pick, the HUD, exposure),
    // from the scene's camera; the first view also makes the occlusion map
    // and the screenshot.
    void drawViews(SDL_GPUCommandBuffer* commands, const Scene& scene, std::span<const View> views);
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
    // A storage buffer for a slot, or an empty one if it hasn't been made:
    // shaders don't read it then (no lights or probes in use), but the slot
    // can't be left unbound.
    SDL_GPUBuffer* storageOr(const FrameBuffer& b) const
    {
        return b.buffer ? b.buffer.get() : emptyStorage_.get();
    }
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
    // `instanceBase`: where `order`'s draw indices are in the instance
    // stream (pushInstances), so neighbours with the same state and mesh
    // are drawn as one; none: one by one.
    void drawObjects(ScenePass& pass, const Scene& scene, std::span<const ObjectDraw* const> order,
                     std::optional<uint32_t> instanceBase = std::nullopt);
    // Puts draw indices into this frame's instance stream (outside render
    // passes); where they start, or nothing if there's no room.
    std::optional<uint32_t> pushInstances(SDL_GPUCommandBuffer* commands,
                                          std::span<const uint32_t> indices);
    // The instance stream: each pass's draw indices, in its draw order, at
    // its own offset, for instanced draws (the vertex shaders read their
    // draw's index per instance). Sized at the start of a frame.
    FrameBuffer instanceStream_;
    uint32_t instanceStreamUsed_ = 0;
    bool instanceStreamCycled_ = false;
    std::vector<uint32_t> instanceOrder_; // kept, with its capacity

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
    void filterProbe(SDL_GPUCommandBuffer* commands, uint32_t cube, bool irradiance);
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
    // The outlines around Scene::highlight and focusHighlight, over `target`.
    void drawOutline(SDL_GPUCommandBuffer* commands, const Scene& scene, SDL_GPUTexture* target,
                     uint32_t width, uint32_t height, const FrameVS& frameVs);
    // One: the draws with this pick id (`faceOnly`: that face, else any of
    // its prim's), outlined in `color`.
    void drawOutline(SDL_GPUCommandBuffer* commands, const Scene& scene, SDL_GPUTexture* target,
                     uint32_t width, uint32_t height, const FrameVS& frameVs, uint64_t pickId,
                     bool faceOnly, const glm::vec4& color);

    // Labels (RendererText.cpp): glyph quads for this frame's labels (and the
    // glyphs new to the atlas) up to the GPU, then drawn over `target`
    // (depth tested against the view), or into the HUD image.
    void uploadLabels(SDL_GPUCommandBuffer* commands, const Scene& scene,
                      const glm::mat4& viewProjection, uint32_t width, uint32_t height);
    void drawLabels(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, bool hud);

    // Water (RendererWater.cpp): this frame's squares up to the GPU; the
    // view so far copied for it to show through; drawn into a view.
    void uploadWater(SDL_GPUCommandBuffer* commands, const Scene& scene);
    void copyForWater(SDL_GPUCommandBuffer* commands, uint32_t width, uint32_t height);
    void drawWater(ScenePass& pass, const Scene& scene, const Camera& camera, uint32_t width,
                   uint32_t height);
    // The water mask (as the official mWaterExclusionMask): the water's
    // squares 1, water exclusion surfaces in front of them (above the
    // water) 0; the scene's shaders read it (waterMaskBinding_: a white
    // texel when nothing excludes water, or outside the view's passes).
    void drawWaterMask(SDL_GPUCommandBuffer* commands, const Scene& scene, const FrameVS& frameVs,
                       uint32_t width, uint32_t height);
    SDL_GPUTexture* waterMask_ = nullptr;
    SDL_GPUTexture* waterMaskDepth_ = nullptr;
    uint32_t waterMaskWidth_ = 0;
    uint32_t waterMaskHeight_ = 0;
    SDL_GPUTextureSamplerBinding waterMaskBinding_{};
    FrameBuffer waterVertices_;
    uint32_t waterVertexCount_ = 0;
    SDL_GPUTexture* waterScene_ = nullptr; // the view behind the water
    uint32_t waterSceneWidth_ = 0;
    uint32_t waterSceneHeight_ = 0;

    // Particles (RendererParticles.cpp): this frame's quads, far ones first
    // (from `camera`), up to the GPU once; drawn into each view after the
    // blended objects, blended as each asks, and their glow into the mask.
    void uploadParticles(SDL_GPUCommandBuffer* commands, const Scene& scene,
                         const glm::vec3& camera);
    void drawParticles(ScenePass& pass);
    void drawParticleGlow(SDL_GPURenderPass* pass);
    // The pipeline for a blend (world::ParticleSystem::Blend factors), made
    // when first needed.
    SDL_GPUGraphicsPipeline* particlePipeline(ScenePipelines& set, uint8_t source, uint8_t dest);
    struct ParticleBatch
    {
        SDL_GPUTexture* texture = nullptr;
        uint8_t blendSource = 0;
        uint8_t blendDest = 0;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        bool glows = false;
    };
    std::vector<ParticleBatch> particleBatches_;
    FrameBuffer particleVertices_;
    GpuBuffer particleIndices_;

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
    // Multisampling: what the scene is drawn into, then resolved into those.
    uint32_t samples_ = 1;
    SDL_GPUTexture* msaaColor_ = nullptr;
    SDL_GPUTexture* msaaDepth_ = nullptr;
    // The depth again as a color (kSceneDepthFormat), and it resolved: what
    // resolveDepth makes depthTarget_ from (SDL can't resolve depth).
    SDL_GPUTexture* msaaSceneDepth_ = nullptr;
    SDL_GPUTexture* sceneDepth_ = nullptr;
    void releaseTargets();
    // Dynamic exposure (as the official viewer's): the scene's luminance,
    // mipmapped down to its average, and the exposure (1x1), last frame's
    // and this one's.
    static constexpr uint32_t kLuminanceSize = 256;
    static constexpr uint32_t kLuminanceLevels = 9; // 256 down to 1
    SDL_GPUTexture* luminance_ = nullptr;
    std::array<SDL_GPUTexture*, 2> exposureMaps_{};
    uint32_t exposureIndex_ = 0;
    // The current exposure: before any is measured (or with dynamic exposure
    // left out from the start), 1.
    SDL_GPUTexture* exposureMap() const
    {
        return exposureMaps_[exposureIndex_] ? exposureMaps_[exposureIndex_] : white_.get();
    }
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
    // Panels' images among the labels' vertices (drawn with their own
    // pipeline and texture, in order).
    struct LabelImage
    {
        uint32_t first = 0;
        uint32_t count = 0;
        SDL_GPUTexture* texture = nullptr;
    };
    std::vector<LabelImage> labelImages_;

    // The eye images (the window's format, as the views' passes draw into),
    // made or remade at this size; false if they couldn't be.
    bool ensureEyeImages(uint32_t width, uint32_t height);
    std::array<SDL_GPUTexture*, 2> eyeImages_{};
    uint32_t eyeWidth_ = 0;
    uint32_t eyeHeight_ = 0;
    // Into a headset's image of this format (made when first needed).
    SDL_GPUGraphicsPipeline* eyeCopyPipeline(SDL_GPUTextureFormat format);
    bool extendedDevice_ = false;

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
        // Each probe is captured twice, as the official viewer's
        // LLReflectionMapManager does: for its radiance (reflections), then
        // for its irradiance (ambient light), with the sky made gray, no
        // ambient light and no clouds, so the world isn't tinted blue.
        bool irradiance = false;
    };
    ProbeCapture capture_;
    FrameBuffer probeBuffer_; // the local probes surfaces use this frame
    // Clusters (Clusters.hpp): the lights' and probes' spheres in the order
    // the shaders have them, and each view's cluster lists, out to this far.
    std::vector<ClusterSphere> clusterLights_;
    std::vector<ClusterSphere> clusterProbes_;
    std::vector<FrameBuffer> clusterBuffers_;
    static constexpr float kClusterFar = 512.0f;
    uint32_t probesShaded_ = 0;

    GpuBuffer emptyStorage_; // stands in for storage buffers not made yet (storageOr)
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
    // Particles whose texture is the default image while it loads: a soft
    // white dot, as the official viewer's pixiesmall.
    GpuTexture particleImage_;

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
    bool screenshotDrawn_ = false; // this frame, to read back at its end
    std::optional<image::MipLevel> screenshot_;
};

} // namespace viewer::render
