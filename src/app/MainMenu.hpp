#pragma once

// The main menu: tabs (Home, Destinations, Inventory, Outfits, Parcel,
// Media, Options, About) and the chosen one's page, an interface page
// (assets/ui/main_menu.rml) shown over the window on the desktop, or on a
// panel in VR (and first person). Home shows the clocks (SLT and local),
// the viewer's and the region's performance and where we are; Destinations,
// the Destination Guide's categories (What's Hot Now first), a category's
// places, and a place alone, to teleport to; About, the
// version and the licenses of everything in the viewer (gathered by the
// build: cmake/Licenses.cmake). The other tabs are still to come.
//
// The app gives it what it shows (update()); what it looks like is the
// documents' (they can be reloaded while running: reload()).

#include "Destinations.hpp"

#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

class MainMenu
{
public:
    // The page's size in pixels (its image's).
    static constexpr glm::ivec2 kSize{1280, 800};

    // Null if the page can't be made.
    static std::unique_ptr<MainMenu> create(gui::Gui& gui, Destinations& destinations);

    bool isOpen() const { return open_; }
    void setOpen(bool open);
    void toggle() { setOpen(!open_); }
    // Shows a tab (its id: "home", "destinations", ...).
    void showTab(const std::string& id)
    {
        tab_ = id;
        model_.DirtyVariable("tab");
    }

    // What Home shows; given each frame while open.
    struct Status
    {
        float viewerFps = 0.0f;
        std::optional<float> regionFps;
        std::optional<float> timeDilation;
        std::string region;   // its name
        std::string position; // where in it
        std::string name;     // ours (first name), for the greeting
        // The maturity of places to show ("g", "gm" or "gma": the
        // resident's preference), and whether we can teleport.
        std::string maturity = "g";
        bool inWorld = false;
    };
    // The page's values, then its layout and animations (once a frame
    // while open, before render()).
    void update(const Status& status);
    // Into its image (outside any render pass).
    void render(SDL_GPUCommandBuffer* commands) { surface_->render(commands); }

    // A destination the page asked to teleport to ("Region/x/y/z",
    // URL-encoded), once.
    std::optional<std::string> takeTeleport() { return std::exchange(teleport_, std::nullopt); }

    gui::Surface& surface() { return *surface_; }
    // Reads the document and its style sheets again.
    void reload();

    struct Tab
    {
        Rml::String id;
        Rml::String name;
    };
    // A destination as the page shows it.
    struct Place
    {
        Rml::String name;
        Rml::String description;
        Rml::String maturity;   // "General", "Moderate", "Adult"
        Rml::String rating;     // "g", "m", "a" (for its style)
        Rml::String image;      // image://..., empty until it's here
        Rml::String largeImage; // the full-sized picture, when the place is shown alone
        bool operator==(const Place&) const = default;
    };
    // A category of the Destination Guide, as the page shows it.
    struct Category
    {
        Rml::String id; // its simple_name
        Rml::String name;
        Rml::String places; // "364 places"
        Rml::String image;
        bool operator==(const Category&) const = default;
    };

private:
    MainMenu() = default;
    void set(const char* variable, std::string& field, std::string value);
    void readLicenses();
    void updateDestinations(const Status& status);

    Destinations* destinations_ = nullptr;
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
    // Destinations: the categories; one's places (category_ isn't empty);
    // one place alone (chosen_ isn't -1). The page picks them.
    std::vector<Category> categories_;
    Rml::String categoriesNote_; // loading, failed
    Rml::String category_;
    Rml::String shownCategory_;
    Rml::String categoryName_;
    Rml::String categoryText_; // its description
    std::string maturity_;     // the places' (Status::maturity)
    bool hasMore_ = false;     // it has more places than those shown
    bool loadingMore_ = false;
    std::vector<Place> places_;
    std::vector<std::string> placeLocations_;
    uint64_t placesVersion_ = ~uint64_t{0};
    Rml::String placesNote_; // loading, failed, none
    int chosen_ = -1;        // the place shown alone (the page picks it); -1: the list
    int shownChosen_ = -2;
    Place pick_; // the chosen one
    bool inWorld_ = false;
    std::optional<std::string> teleport_;
    // About.
    std::string version_;
    std::string builtWith_;
    std::vector<Rml::String> licenseNames_;
    std::vector<std::string> licenses_;
    int license_ = 0; // which is shown (the page picks it)
    int shownLicense_ = -1;
    std::string licenseText_;
};

} // namespace viewer
