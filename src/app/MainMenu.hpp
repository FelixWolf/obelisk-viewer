#pragma once

// The main menu: tabs (Home, Destinations, Inventory, Outfits, Parcel,
// Media, Options) and the chosen one's page, an interface page
// (assets/ui/main_menu.rml) shown over the window on the desktop, or on a
// panel in VR (and first person). Home shows the clocks (SLT and local),
// the viewer's and the region's performance and where we are; the other
// tabs are still to come.
//
// The app gives it what it shows (update()); what it looks like is the
// documents' (they can be reloaded while running: reload()).

#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace viewer
{

class MainMenu
{
public:
    // The page's size in pixels (its image's).
    static constexpr glm::ivec2 kSize{1280, 800};

    // Null if the page can't be made.
    static std::unique_ptr<MainMenu> create(gui::Gui& gui);

    bool isOpen() const { return open_; }
    void setOpen(bool open);
    void toggle() { setOpen(!open_); }

    // What Home shows; given each frame while open.
    struct Status
    {
        float viewerFps = 0.0f;
        std::optional<float> regionFps;
        std::optional<float> timeDilation;
        std::string region;   // its name
        std::string position; // where in it
        std::string name;     // ours (first name), for the greeting
    };
    // The page's values, then its layout and animations (once a frame
    // while open, before render()).
    void update(const Status& status);
    // Into its image (outside any render pass).
    void render(SDL_GPUCommandBuffer* commands) { surface_->render(commands); }

    gui::Surface& surface() { return *surface_; }
    // Reads the document and its style sheets again.
    void reload();

    struct Tab
    {
        Rml::String id;
        Rml::String name;
    };

private:
    MainMenu() = default;
    void set(const char* variable, std::string& field, std::string value);

    std::unique_ptr<gui::Surface> surface_;
    Rml::DataModelHandle model_;
    bool open_ = false;

    std::vector<Tab> tabs_;
    Rml::String tab_ = "home";
    Rml::String tabName_;
    float viewerFps_ = 0.0f;
    float regionFps_ = 0.0f;
    float dilation_ = 1.0f;
    std::string viewerFpsText_;
    std::string regionFpsText_;
    std::string dilationText_;
    std::string slt_;
    std::string sltDate_;
    std::string local_;
    std::string localDate_;
    std::string region_;
    std::string position_;
    std::string greeting_;
    double lastNumbers_ = -1.0; // when the numbers were last shown (seconds)
};

} // namespace viewer
