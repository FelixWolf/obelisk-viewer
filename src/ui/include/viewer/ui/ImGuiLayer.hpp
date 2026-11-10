#pragma once

#include "viewer/render/Renderer.hpp"

#include <SDL3/SDL_events.h>

#include <memory>

struct ImGuiContext;

namespace viewer::ui {

// Owns an ImGui context and its SDL3 + SDL GPU backends.
//
// ImGui itself keeps a "current context" pointer internally; we create our own
// context explicitly and own its lifetime here rather than relying on a
// process-wide default.
class ImGuiLayer {
public:
    static std::unique_ptr<ImGuiLayer> create(SDL_Window* window, render::Renderer& renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Returns true if ImGui wants exclusive use of this event (the world
    // should not also react to it).
    bool processEvent(const SDL_Event& event);

    void beginFrame();

    // Finalizes the ImGui frame. Must be called once per beginFrame().
    void endFrame();

    // Records the UI on top of whatever is already in frame.target.
    void record(const render::Frame& frame);

private:
    explicit ImGuiLayer(ImGuiContext* context);

    ImGuiContext* context_ = nullptr;
};

} // namespace viewer::ui
