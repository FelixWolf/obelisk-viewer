#pragma once

#include "CameraController.hpp"
#include "LoginPanel.hpp"
#include "SceneAssets.hpp"
#include "TerrainTextures.hpp"
#include "TerrainView.hpp"
#include "TextureBrowser.hpp"
#include "TextureManager.hpp"

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/ui/ImGuiLayer.hpp"
#include "viewer/world/World.hpp"

#include <SDL3/SDL_video.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

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
    std::filesystem::path screenshotPath;       // optional: save a screenshot, then quit
    float screenshotDelay = 3.0f;               // seconds after startup
    std::optional<std::array<float, 6>> camera; // x y z distance yaw pitch
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
    void handleLoginAction(LoginAction action);
    void loadTerrainBlendRamp();
    void updateSession();
    void logOutBeforeExit();
    render::Scene buildScene();
    void endSession();

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
    std::unique_ptr<assets::TextureCache> textureCache_;      // texture browser panel
    std::unique_ptr<assets::TextureCache> worldTextureCache_; // textures of the world
    std::unique_ptr<TextureBrowser> textureBrowser_;
    std::unique_ptr<net::HttpClient> http_;
    std::unique_ptr<TextureManager> textures_;
    TerrainTextures terrainTextures_;
    std::unique_ptr<LoginPanel> loginPanel_;
    std::unique_ptr<SceneAssets> sceneAssets_;
    std::unique_ptr<session::Session> session_;
    // Session-lifetime, created and destroyed together with session_.
    std::unique_ptr<world::World> world_;
    TerrainView terrainView_;
    std::vector<session::RegionMessage> regionMessages_;
    std::optional<uint64_t> originHandle_; // world coordinates are relative to this region

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
};

} // namespace viewer
