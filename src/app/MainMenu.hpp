#pragma once

// The main menu: tabs (Home, Destinations, Inventory, Outfits, Parcel,
// Media, Settings, About) and the chosen one's page, an interface page
// (assets/ui/main_menu.rml) shown over the window on the desktop, or on a
// panel in VR (and first person). Home shows the clocks (SLT and local),
// the viewer's and the region's performance and where we are (with a map
// of our region and its neighbours, and who's on it: RegionMap); Destinations,
// the Destination Guide's categories (What's Hot Now first), a category's
// places, and a place alone, to teleport to; Parcel, the one we're on (its
// picture, owner, group, size, rules, description, and its estate);
// Inventory, its folders as a file explorer shows them, and an item's
// details; Chat, local chat and instant messages; People, friends and
// who's nearby; Media, the parcel's music (playing it, what's on) and
// video, and the media on prims around; Settings, the viewer's settings
// page by page (SettingsView); About, the
// version and the licenses of everything in the viewer (gathered by the
// build: cmake/Licenses.cmake). The other tabs are still to come.
//
// The app gives it what it shows (update()); what it looks like is the
// documents' (they can be reloaded while running: reload()).

#include "ChatView.hpp"
#include "CurrentOutfit.hpp"
#include "Destinations.hpp"
#include "GridPopulation.hpp"
#include "InventoryBrowser.hpp"
#include "MusicStream.hpp"
#include "ObjectMedia.hpp"
#include "ParcelInfo.hpp"
#include "PeopleView.hpp"
#include "RegionMap.hpp"
#include "SettingsView.hpp"

#include "viewer/core/Branding.hpp"
#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
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
    static std::unique_ptr<MainMenu> create(gui::Gui& gui, Destinations& destinations,
                                            GridPopulation& population, InventoryBrowser& inventory,
                                            Settings& settings);
    // Names of avatars (creators, ...): empty until known ("Unknown" if
    // there's none).
    void setAvatarNames(std::function<std::string(const Uuid&)> names)
    {
        avatarNames_ = std::move(names);
    }

    bool isOpen() const { return open_; }
    void setOpen(bool open);
    void toggle() { setOpen(!open_); }
    // Shows a tab (its id: "home", "destinations", ...).
    void showTab(const std::string& id)
    {
        tab_ = id;
        model_.DirtyVariable("tab");
    }
    const std::string& tab() const { return tab_; }

    // What Home shows; given each frame while open.
    struct Status
    {
        float viewerFps = 0.0f;
        std::optional<float> regionFps;
        std::optional<float> timeDilation;
        std::string region;   // its name
        std::string position; // where in it
        std::string name;     // ours (first name), for the greeting
        // The header's: our display name, profile picture (null: none or
        // not known yet) and L$ (nothing: not heard yet).
        std::string displayName;
        Uuid picture;
        std::optional<int32_t> balance;
        // The maturity of places to show ("g", "gm" or "gma": the
        // resident's preference), and whether we can teleport.
        std::string maturity = "g";
        bool inWorld = false;
        // The parcel we're on (nothing: not known), its owner's and
        // group's names (empty: not known yet), and its estate's.
        const Parcel* parcel = nullptr;
        std::string parcelOwner;
        std::string parcelGroup;
        const ParcelInfo::Estate* estate = nullptr;
        std::string estateOwner;
        // What we wear (attachments' items), and those being put on or
        // taken off.
        const std::unordered_set<Uuid>* worn = nullptr;
        std::function<bool(const Uuid&)> changing;
        // Clothing and body parts worn (linked in the Current Outfit Folder).
        std::function<bool(const Uuid&)> wearing;
        // The outfit worn last (null: none), and an outfit being put on.
        Uuid baseOutfit;
        bool outfitChanging = false;
        // Local chat heard, instant messages, our id, and whether we're in
        // a headset (typing on the VR keyboard).
        const LocalChat* chat = nullptr;
        const InstantMessages* ims = nullptr;
        Uuid self;
        bool headset = false;
        // Friends and who's around (People), their names, and where we are
        // on the grid (nothing: not in the world).
        const Friends* friends = nullptr;
        const Nearby* nearby = nullptr;
        PeopleView::Names names;
        std::optional<glm::dvec3> here;
        // Home's map of the regions around ours (null: not shown).
        const RegionMap* regionMap = nullptr;
        // Media: the parcel's music stream, whether it starts by itself,
        // how a script last said the parcel's video should play, and the
        // prims around with media (nearest first; their names empty until
        // known).
        const MusicStream* music = nullptr;
        bool musicAutoPlay = true;
        const ParcelInfo::MediaPlay* parcelMediaPlay = nullptr;
        // Voice, in words (empty: not in the world), and the microphone.
        std::string voice;
        PeopleView::Voice voiceStatus;
        PeopleView::RenderingOf rendering; // how those around are drawn
        bool microphone = false;
        struct PrimMedia
        {
            Uuid object;
            std::string name;
            float distance = 0.0f; // meters
            const ObjectMediaData* media = nullptr;
        };
        std::vector<PrimMedia> primMedia;
        // What a face's media is doing ("Playing", "Click to play"; empty:
        // nothing to say), by object and face.
        std::function<std::string(const Uuid&, size_t)> primMediaState;
        size_t primMediaTotal = 0; // all known (more than listed)
    };
    // The page's values, then its layout and animations (once a frame
    // while open, before render()).
    void update(const Status& status);
    // Into its image (outside any render pass).
    void render(SDL_GPUCommandBuffer* commands) { surface_->render(commands); }

    // A destination the page asked to teleport to ("Region/x/y/z",
    // URL-encoded), once.
    std::optional<std::string> takeTeleport() { return std::exchange(teleport_, std::nullopt); }
    // Log out was pressed, once.
    bool takeLogOut() { return std::exchange(logOut_, false); }
    // An item the page asked to attach or take off, once.
    struct InventoryAction
    {
        enum class Kind
        {
            Attach,
            Detach,
            Wear,       // clothing or a body part, instead of others of its kind
            Add,        // clothing, over what's worn
            TakeOff,    // clothing
            WearOutfit, // an outfit (a folder) instead of what's worn
        };
        Kind kind = Kind::Attach;
        Uuid item; // the item itself (not a link to it), or the outfit's folder
    };
    std::optional<InventoryAction> takeInventoryAction()
    {
        return std::exchange(inventoryAction_, std::nullopt);
    }
    // What the Media tab asked for, once.
    enum class MediaAction
    {
        PlayMusic,
        StopMusic,
        Louder,
        Quieter,
    };
    std::optional<MediaAction> takeMediaAction()
    {
        return std::exchange(mediaAction_, std::nullopt);
    }

    // The header's voice key was pressed (the microphone, on or off), once.
    bool takeMicrophone() { return std::exchange(microphonePressed_, false); }

    gui::Surface& surface() { return *surface_; }
    // The Chat tab: what it said, and whether it wants the VR keyboard.
    ChatView& chat() { return chat_; }
    // The People tab: what it asked for.
    PeopleView& people() { return people_; }
    // The Settings tab: whether it wants the VR keyboard.
    SettingsView& settings() { return *settings_; }
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
        Rml::String people;     // "12 people" (from the map), empty until known
        bool operator==(const Place&) const = default;
    };
    // A line of an inventory folder, as the page shows it.
    struct InvEntry
    {
        Rml::String name;
        Rml::String kind; // "Texture", "Folder", ...
        Rml::String icon; // icons/<icon>.svg
        bool link = false;
        bool broken = false;
        bool worn = false;
        bool operator==(const InvEntry&) const = default;
    };
    // An item, shown alone.
    struct InvItem
    {
        Rml::String name;
        Rml::String kind;
        Rml::String icon;
        Rml::String description;
        Rml::String creator;
        Rml::String permissions; // "Copy · Modify · Transfer"
        Rml::String created;
        Rml::String image; // asset://... for textures; empty: none
        Rml::String link;  // "A link" when it's one
        bool attachable = false;
        bool clothing = false; // can be added over others, and taken off
        bool bodyPart = false;
        bool worn = false;
        bool changing = false; // being put on or taken off
        bool operator==(const InvItem&) const = default;
    };
    // An outfit, as the page shows it.
    struct OutfitCard
    {
        Rml::String name;
        Rml::String image; // asset://... its thumbnail; empty: none
        bool worn = false;
        bool operator==(const OutfitCard&) const = default;
    };
    // A face's media, as the Media tab lists it (the first of an object's
    // has its name and distance).
    struct MediaRow
    {
        Rml::String object; // empty: the same as the row before
        Rml::String where;
        Rml::String face;
        Rml::String url;
        Rml::String details;
        bool operator==(const MediaRow&) const = default;
    };
    // One of a parcel's rules, as the page shows it.
    struct Rule
    {
        Rml::String name;
        Rml::String state; // "yes", "no", "group" (for its style)
        Rml::String text;  // "Allowed", ...
        bool operator==(const Rule&) const = default;
    };
    // A region on Home's map, as the page shows it (the northern row
    // first).
    struct MapTile
    {
        Rml::String name;  // empty: not known
        Rml::String image; // empty: water
        bool here = false; // ours
        bool operator==(const MapTile&) const = default;
    };
    // Someone on Home's map: where ("37.5%" across from the left and down
    // from the top) and who ("self", "friend" or "other", for its style).
    struct MapDot
    {
        Rml::String left;
        Rml::String top;
        Rml::String kind;
        bool operator==(const MapDot&) const = default;
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
    void updateParcel(const Status& status);
    void updateMedia(const Status& status);
    void updateInventory(const Status& status);
    void openInventoryEntry(int index);
    void updateOutfits(const Status& status);
    void updateMap(const Status& status);

    Destinations* destinations_ = nullptr;
    GridPopulation* population_ = nullptr;
    InventoryBrowser* inventory_ = nullptr;
    std::function<std::string(const Uuid&)> avatarNames_;
    std::vector<std::string> placeRegions_;
    uint64_t populationVersion_ = ~uint64_t{0};
    double populationAt_ = -1.0; // when the numbers were last looked at (seconds)
    std::unique_ptr<gui::Surface> surface_;
    bool logOut_ = false;
    Rml::DataModelHandle model_;
    ChatView chat_;
    PeopleView people_;
    std::unique_ptr<SettingsView> settings_;
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
    std::string displayName_;   // the header's
    std::string picture_;       // its picture's source (empty: none)
    std::string balance_;       // "L$1,234" (empty: not known)
    std::string voice_;         // the header's
    bool microphone_ = false;
    bool microphonePressed_ = false;
    double lastNumbers_ = -1.0; // when the numbers were last shown (seconds)
    // Home's map: its regions, and the people on it.
    std::vector<MapTile> mapTiles_;
    std::vector<MapDot> mapDots_;
    bool mapShown_ = false;
    uint64_t mapVersion_ = ~uint64_t{0};
    double mapDotsAt_ = -1.0; // when the people were last placed (seconds)
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
    // Inventory: the folders opened from the root, and an item shown alone
    // (null: none). The page asks (invOpen, invUp, invBack).
    std::vector<Uuid> invPath_;
    Uuid invShown_;
    std::vector<InventoryEntry> invEntriesKnown_; // the lines, as the page has them
    Uuid invFolderShown_;
    uint64_t invVersion_ = ~uint64_t{0};
    size_t invPathShown_ = 0;
    std::vector<Rml::String> invCrumbs_;
    std::vector<InvEntry> invEntries_;
    Rml::String invNote_;
    bool invItemShown_ = false;
    InvItem invItem_;
    Uuid invTarget_; // the item shown (a link's target)
    std::unordered_set<Uuid> invWornShown_;
    double invWornAt_ = -1.0; // when the lines' worn marks were last looked at (seconds)
    std::optional<InventoryAction> inventoryAction_;
    // Outfits: those in My Outfits, and one shown alone (null: none).
    std::vector<Uuid> outfitIds_;
    std::vector<OutfitCard> outfitCards_;
    Rml::String outfitsNote_;
    uint64_t outfitsVersion_ = ~uint64_t{0};
    Uuid outfitShown_;
    Uuid outfitShownBuilt_; // the one the page's items are of
    Uuid outfitBaseShown_;  // the outfit worn, as shown
    bool outfitPage_ = false;
    OutfitCard outfit_;
    std::vector<InvEntry> outfitItems_;
    Rml::String outfitNote_;
    bool outfitChanging_ = false;
    // Parcel.
    bool parcelKnown_ = false;
    Rml::String parcelName_;
    Rml::String parcelWhere_; // its region (and category)
    Rml::String parcelText_;  // its description
    Rml::String parcelImage_; // asset://..., empty: none
    Rml::String parcelOwner_;
    Rml::String parcelGroup_;
    Rml::String parcelArea_;
    Rml::String parcelPrims_;
    Rml::String parcelSale_; // empty: not for sale
    Rml::String parcelMusic_;
    std::vector<Rule> parcelRules_;
    Rml::String estateName_;
    Rml::String estateOwner_;    // empty: none said (mainland)
    Rml::String estateCovenant_; // whether it has one
    // Media.
    Rml::String musicUrl_; // empty: the parcel has none
    Rml::String musicState_;
    Rml::String musicDetail_; // format, rate, bitrate
    Rml::String musicStation_;
    Rml::String musicTitle_;
    Rml::String musicError_;
    bool musicOn_ = false; // playing (or trying to)
    int musicVolume_ = 50; // percent
    Rml::String musicAuto_;
    Rml::String videoUrl_; // empty: none
    Rml::String videoDetail_;
    Rml::String videoNote_;
    std::vector<MediaRow> primMedia_;
    Rml::String primMediaNote_;
    std::optional<MediaAction> mediaAction_;
    // About.
    std::string appName_{branding::kName};
    std::string version_;
    std::string builtWith_;
    std::vector<Rml::String> licenseNames_;
    std::vector<std::string> licenses_;
    int license_ = 0; // which is shown (the page picks it)
    int shownLicense_ = -1;
    std::string licenseText_;
};

} // namespace viewer
