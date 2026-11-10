#include "App.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

namespace viewer {

std::unique_ptr<App> App::create()
{
    std::unique_ptr<App> app(new App());

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        log::error("SDL_Init failed: {}", SDL_GetError());
        return nullptr;
    }
    app->sdlInitialized_ = true;

    app->window_.reset(SDL_CreateWindow("Viewer", 1600, 900,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!app->window_) {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return nullptr;
    }

    render::RendererOptions renderOptions;
#ifdef VIEWER_GPU_DEBUG
    renderOptions.gpuDebug = true;
#endif
    app->renderer_ = render::Renderer::create(app->window_.get(), renderOptions);
    if (!app->renderer_) {
        return nullptr;
    }

    app->ui_ = ui::ImGuiLayer::create(app->window_.get(), *app->renderer_);
    if (!app->ui_) {
        return nullptr;
    }

    return app;
}

App::~App()
{
    ui_.reset();
    renderer_.reset();
    window_.reset();
    if (sdlInitialized_) {
        SDL_Quit();
    }
}

int App::run()
{
    while (running_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool uiConsumed = ui_->processEvent(event);
            (void)uiConsumed; // later: forward unconsumed input to the camera/avatar

            if (event.type == SDL_EVENT_QUIT) {
                running_ = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window_.get())) {
                running_ = false;
            }
        }

        if (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ui_->beginFrame();
        drawUi();
        ui_->endFrame();

        if (auto frame = renderer_->beginFrame()) {
            renderer_->drawScene(*frame);
            ui_->record(*frame);
        }
        renderer_->endFrame();
    }
    return 0;
}

void App::drawUi()
{
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::Begin("Stats")) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.2f ms)", static_cast<double>(io.Framerate),
                    static_cast<double>(1000.0f / io.Framerate));
        ImGui::Text("GPU driver: %s", renderer_->driverName());
        ImGui::Checkbox("ImGui demo window", &showDemoWindow_);
    }
    ImGui::End();

    if (showDemoWindow_) {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

} // namespace viewer
