#pragma once

#include "viewer/render/Renderer.hpp"

#include <SDL3/SDL_events.h>

#include <filesystem>
#include <memory>
#include <string>

struct ImGuiContext;

namespace viewer::ui
{

// Owns an ImGui context and its SDL3 + SDL GPU backends.
//
// ImGui itself keeps a "current context" pointer internally; we create our own
// context explicitly and own its lifetime here rather than relying on a
// process-wide default.
class ImGuiLayer
{
public:
    // `iniFile`: where ImGui keeps its windows' places and sizes (in the
    // user's settings folder, not the working directory); empty, nowhere.
    static std::unique_ptr<ImGuiLayer> create(SDL_Window* window, render::Renderer& renderer,
                                              const std::filesystem::path& iniFile);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Returns true if ImGui wants exclusive use of this event (the world
    // should not also react to it).
    bool processEvent(const SDL_Event& event);

    void beginFrame();

    // Whether the UI may use the mouse: not while it's captured for first
    // person. Then it neither captures it (SDL_CaptureMouse; on X11
    // releasing that also lets go of the pointer's grab) nor sets the
    // cursor (on Wayland that shows it, and can undo the pointer's lock).
    void setMouseCapture(bool allowed);

    // Finalizes the ImGui frame. Must be called once per beginFrame().
    void endFrame();

    // Records the UI on top of whatever is already in frame.target.
    void record(const render::Frame& frame);

private:
    explicit ImGuiLayer(ImGuiContext* context);

    ImGuiContext* context_ = nullptr;
    std::string iniFile_; // io.IniFilename points into it
};

} // namespace viewer::ui
