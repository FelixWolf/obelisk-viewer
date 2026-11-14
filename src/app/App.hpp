#pragma once

#include "AgentController.hpp"
#include "AnimationManager.hpp"
#include "AvatarNames.hpp"
#include "AvatarView.hpp"
#include "CameraController.hpp"
#include "EnvironmentManager.hpp"
#include "FrameProfile.hpp"
#include "LegacyMaterialManager.hpp"
#include "LocalChat.hpp"
#include "LoginPanel.hpp"
#include "MaterialManager.hpp"
#include "ObjectProperties.hpp"
#include "ObjectView.hpp"
#include "PointerController.hpp"
#include "SceneAssets.hpp"
#include "TerrainTextures.hpp"
#include "TerrainView.hpp"
#include "TextureBrowser.hpp"
#include "TextureManager.hpp"
#include "TreeView.hpp"

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/ui/ImGuiLayer.hpp"
#include "viewer/world/World.hpp"
#include "viewer/world/WorldThread.hpp"

#include <SDL3/SDL_video.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

namespace viewer
{

struct AppOptions
{
    std::filesystem::path cacheDirectory;       // empty = platform default
    std::filesystem::path importDirectory;      // optional: texture browser import on startup
    std::string loginUri;                       // empty = Second Life main grid
    std::filesystem::path loginFile;            // optional: log in at startup (see Credentials.hpp)
    bool testScene = false;                     // show the material test scene
    bool pbrTerrain = false;                    // start with the PBR terrain shader
    bool vsync = true;                          // --no-vsync: uncapped, for measuring
    std::string start;                          // login start: last, home, Region[/x/y/z]
    std::filesystem::path screenshotPath;       // optional: save a screenshot, then quit
    float screenshotDelay = 3.0f;               // seconds after startup
    std::optional<std::array<float, 6>> camera; // x y z distance yaw pitch
    std::string teleport;                       // teleport here 5 s after logging in ("home" too)
    std::optional<float> walk;     // walk this way (degrees from east) once in (after --teleport)
    std::optional<float> dayTime;  // a fixed time of day, 0..1 (0.5 is midday in most cycles)
    bool shadows = true;           // --no-shadows: off (for comparing)
    uint32_t textureMemory = 2048; // --texture-memory: GPU memory for textures, MiB
    // --bandwidth: downstream bandwidth to ask regions for, kbps (the official
    // viewer's ThrottleBandwidthKBPS)
    float bandwidth = session::kDefaultBandwidthKbps;
    // For testing: point at this spot (fractions of the window, from the
    // top left) instead of the mouse, and click there this many seconds in.
    std::optional<std::array<float, 2>> pointer;
    std::optional<float> clickAt;
    Uuid pointAt; // or at this object (its center)
    // For testing: say this in local chat this many seconds in (typing
    // for 3 seconds before).
    std::string say;
    float sayAt = 20.0f;
    bool occlusion = true; // --no-occlusion: draw what's hidden too
};

// Parses command-line arguments; returns nullopt (after printing usage) on error.
std::optional<AppOptions> parseArguments(std::span<char*> args);

// Top-level owner of everything in the process. There are no singletons: every
// subsystem is a member here (or of something owned here) and is handed
// explicitly to whatever needs it.
//
// Lifetimes are split in two:
//   - App-lifetime: window, renderer, UI, job system, asset cache, HTTP
//     client, and later audio. These survive logging out and back in.
//   - Session-lifetime: login data and capabilities now; the UDP circuit,
//     event queue and world state later. Held as std::unique_ptr<Session>;
//     logging out destroys it, logging in builds a fresh one. Nothing
//     App-lifetime may hold pointers into a Session.
class App
{
public:
    static std::unique_ptr<App> create(const AppOptions& options);
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    App() = default;

    void drawUi();
    void drawFpsOverlay();
    void handleLoginAction(LoginAction action);
    void loadTerrainBlendRamp();
    void updateSession();
    void logOutBeforeExit();
    // Fills the frame's scene (cleared first).
    void buildScene(render::Scene& scene);
    void endSession();
    // The agent arrived (login, teleport, region crossing): move the world
    // origin and the camera.
    void handleArrival(const session::Arrival& arrival);
    // Asks for the appearance of avatars that still have none (and so are
    // drawn with default shape parameters).
    void requestMissingAppearances();
    // Every 30 s: the process's resident memory and what the caches hold.
    void logMemory(uint64_t now);
    // Our avatar's position, in global meters; nothing until we see it.
    std::optional<glm::dvec3> agentGlobalPosition();
    void drawTeleportWindow();
    void drawEnvironmentWindow();

    struct SdlRuntime
    {
        bool initialized = false;
        ~SdlRuntime();
    };
    struct WindowDeleter
    {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };

    // Members are destroyed in reverse declaration order, which is the
    // shutdown order: session (cancels its HTTP requests) -> login panel ->
    // HTTP client -> browser (GPU textures) -> cache (waits for its jobs) ->
    // decoder -> job threads -> UI -> renderer -> window -> SDL_Quit.
    SdlRuntime sdl_;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<ui::ImGuiLayer> ui_;
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<image::J2kDecoder> j2kDecoder_;
    std::unique_ptr<DiskCache> diskCache_;                    // before everything using it
    std::unique_ptr<assets::TextureCache> textureCache_;      // texture browser panel
    std::unique_ptr<assets::TextureCache> worldTextureCache_; // textures of the world
    std::unique_ptr<TextureBrowser> textureBrowser_;
    std::unique_ptr<net::HttpClient> http_;
    std::unique_ptr<TextureManager> textures_;
    std::unique_ptr<EnvironmentManager> environment_;
    std::unique_ptr<MeshManager> meshes_;
    std::unique_ptr<MaterialManager> materials_;
    std::unique_ptr<LegacyMaterialManager> legacyMaterials_;
    std::unique_ptr<AvatarNames> avatarNames_;     // display names, for name tags
    ObjectMotion motion_;                          // before the views that read it
    std::unique_ptr<AnimationManager> animations_; // with avatars_
    std::unique_ptr<AvatarView> avatars_; // nullptr if the default avatar couldn't be loaded
    std::unique_ptr<ObjectView> objects_;
    // What the mouse points at, and touching it.
    PointerController pointer_;
    // Names and touch text of what the pointer rests on.
    ObjectProperties objectProperties_;
    std::optional<world::ObjectKey> hoverKey_;
    uint64_t hoverSinceTicks_ = 0;
    const ObjectInfo* hoverInfo_ = nullptr;
    std::optional<std::array<float, 2>> pointerOverride_;
    Uuid pointAt_;
    bool loggedHoverInfo_ = false;
    std::optional<float> clickAt_;
    std::string say_; // --say, and when
    float sayAt_ = 0.0f;
    int said_ = 0;          // 1: typing, 2: said
    bool occlusion_ = true; // occlusion culling
    LocalChat localChat_;
    uint64_t clickedTicks_ = 0; // --click-at's press, released a moment later
    // The pointer's ray this frame (none over the UI or while dragging).
    std::optional<PointerController::Ray> pointerRay(const render::Scene& scene) const;
    void pressPointer();
    std::unique_ptr<TreeView> trees_;
    TerrainTextures terrainTextures_;
    std::unique_ptr<LoginPanel> loginPanel_;
    std::unique_ptr<SceneAssets> sceneAssets_;
    std::unique_ptr<session::Session> session_;
    // Session-lifetime, created and destroyed together with session_. The
    // world thread owns the world; world_ is the snapshot this frame reads.
    std::unique_ptr<world::WorldThread> worldThread_;
    std::shared_ptr<const world::World> world_;
    TerrainView terrainView_;
    std::vector<session::RegionMessage> regionMessages_;
    std::vector<world::WorldRequest> worldRequests_;
    std::vector<std::pair<uint32_t, uint8_t>> objectRequests_;
    std::optional<uint64_t> originHandle_; // world coordinates are relative to this region
    uint64_t seenArrivals_ = 0;
    std::optional<world::ObjectKey> agentKey_; // our avatar's object, when last found
    uint64_t agentSearchRevision_ = ~0ull;     // objects revision we last searched
    std::optional<glm::dvec3> lastAgentGlobal_;
    // Avatars without an appearance: when to ask for it (next).
    std::unordered_map<Uuid, std::chrono::steady_clock::time_point> appearanceDue_;
    std::chrono::steady_clock::time_point nextAppearanceCheck_{};
    AgentController agent_;
    std::string pendingTeleport_;      // --teleport, once we're in
    std::optional<float> walkHeading_; // --walk
    uint64_t arrivedTicks_ = 0;
    uint64_t lastMemoryLog_ = 0;
    std::array<char, 256> teleportInput_{};

    CameraController camera_;
    bool running_ = true;
    bool showDemoWindow_ = false;
    bool showTestScene_ = false;
    bool pbrTerrain_ = false;
    bool fixedCamera_ = false; // --camera given: don't move it to the agent
    std::filesystem::path screenshotPath_;
    float screenshotDelay_ = 0.0f;
    bool screenshotRequested_ = false;
    uint64_t startTicks_ = 0;
    uint64_t lastFrameTicks_ = 0;
    // Recent frame times (milliseconds, oldest first) for the FPS overlay.
    std::vector<float> frameTimes_;
    FrameProfile profile_;
    render::Scene scene_; // this frame's, kept for its lists' capacity
    bool showFps_ = true;
    bool shadows_ = true;
    // Where HUD attachments show (render::Hud): over the screen, on the
    // left forearm, or floating in front of the camera.
    enum class HudPlacement : int
    {
        Screen,
        Forearm,
        Floating,
    };
    HudPlacement hudPlacement_ = HudPlacement::Screen;
    // The HUD panel's frame for the placements in the scene.
    void placeHud(render::Scene& scene);
    float bandwidth_ = session::kDefaultBandwidthKbps; // kbps, for each new session
};

} // namespace viewer
