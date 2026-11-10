#pragma once

#include "viewer/render/Renderer.hpp"
#include "viewer/ui/ImGuiLayer.hpp"

#include <SDL3/SDL_video.h>

#include <memory>

namespace viewer {

// Top-level owner of everything in the process. There are no singletons: every
// subsystem is a member here (or of something owned here) and is handed
// explicitly to whatever needs it.
//
// Lifetimes are split in two:
//   - App-lifetime: window, renderer, UI, and later the job system, asset
//     cache and audio. These survive logging out and back in.
//   - Session-lifetime (not yet implemented): the UDP circuit, capabilities,
//     event queue, and the world state for one login. Held as
//     std::unique_ptr<Session>; disconnecting destroys it, reconnecting builds
//     a fresh one. Nothing App-lifetime may hold pointers into a Session.
class App {
public:
    static std::unique_ptr<App> create();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    App() = default;

    void drawUi();

    struct WindowDeleter {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };

    // Declaration order matters: destroyed in reverse, so the UI goes before
    // the renderer, which goes before the window.
    bool sdlInitialized_ = false;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<ui::ImGuiLayer> ui_;

    bool running_ = true;
    bool showDemoWindow_ = false;
};

} // namespace viewer
