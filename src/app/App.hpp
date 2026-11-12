#pragma once

#include "AgentController.hpp"
#include "AnimationManager.hpp"
#include "AvatarView.hpp"
#include "CameraController.hpp"
#include "EnvironmentManager.hpp"
#include "FrameProfile.hpp"
#include "LoginPanel.hpp"
#include "MaterialManager.hpp"
#include "ObjectView.hpp"
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
    std::optional<float> walk;    // walk this way (degrees from east) once in (after --teleport)
    std::optional<float> dayTime; // a fixed time of day, 0..1 (0.5 is midday in most cycles)
    bool shadows = true;          // --no-shadows: off (for comparing)
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
    ObjectMotion motion_;                          // before the views that read it
    std::unique_ptr<AnimationManager> animations_; // with avatars_
    std::unique_ptr<AvatarView> avatars_; // nullptr if the default avatar couldn't be loaded
    std::unique_ptr<ObjectView> objects_;
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
};

} // namespace viewer
