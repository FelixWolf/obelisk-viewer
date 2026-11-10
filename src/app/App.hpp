#pragma once

#include "TextureBrowser.hpp"

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/ui/ImGuiLayer.hpp"

#include <SDL3/SDL_video.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace viewer
{

struct AppOptions
{
    std::filesystem::path cacheDirectory;  // empty = platform default
    std::filesystem::path importDirectory; // optional: texture browser import on startup
};

// Parses command-line arguments; returns nullopt (after printing usage) on error.
std::optional<AppOptions> parseArguments(std::span<char*> args);

// Top-level owner of everything in the process. There are no singletons: every
// subsystem is a member here (or of something owned here) and is handed
// explicitly to whatever needs it.
//
// Lifetimes are split in two:
//   - App-lifetime: window, renderer, UI, job system, asset cache, and later
//     audio. These survive logging out and back in.
//   - Session-lifetime (not yet implemented): the UDP circuit, capabilities,
//     event queue, and the world state for one login. Held as
//     std::unique_ptr<Session>; disconnecting destroys it, reconnecting builds
//     a fresh one. Nothing App-lifetime may hold pointers into a Session.
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
    // shutdown order: browser (GPU textures) -> cache (waits for its jobs) ->
    // decoder -> job threads -> UI -> renderer -> window -> SDL_Quit.
    SdlRuntime sdl_;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<ui::ImGuiLayer> ui_;
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<image::J2kDecoder> j2kDecoder_;
    std::unique_ptr<assets::TextureCache> textureCache_;
    std::unique_ptr<TextureBrowser> textureBrowser_;

    bool running_ = true;
    bool showDemoWindow_ = false;
};

} // namespace viewer
