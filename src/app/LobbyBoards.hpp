#pragma once

// The lobby's boards: the news and the grid's status (from viewerconfig),
// each an interface page (assets/ui/news.rml, grid_status.rml) on a panel
// standing where the home scene's markers say, pointed at as the login
// panel is (VrControls' standing pages). A news entry clicked shows whole
// on its board (our posts' text, a feed entry's summary), with its link to
// open in the web browser; the grid status links to Linden Lab's status
// page and its incidents. There's no browser in the viewer: links open in
// the system's, on the desktop (in a headset, the page says so).
//
// The news' pictures come from the web, decoded on the workers and given
// to the pages as image://news-N.

#include "ViewerConfig.hpp"

#include "viewer/core/Branding.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/gui/Gui.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Scene.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <glm/glm.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

// "just now", "5 minutes ago", "3 hours ago", "2 days ago", then the date
// ("Nov 18", with its year if not this one); empty if it can't be read.
std::string describeTime(std::string_view iso, std::chrono::system_clock::time_point now);

class LobbyBoards
{
public:
    // The pages' size in pixels (the login panel's), and their scale.
    static constexpr glm::ivec2 kSize{720, 900};
    static constexpr float kMetersPerPixel = 0.00125f;

    // Null if the pages can't be set up.
    static std::unique_ptr<LobbyBoards> create(gui::Gui& gui, net::HttpClient& http,
                                               JobSystem& jobs, const ViewerConfig& source);
    ~LobbyBoards();
    LobbyBoards(const LobbyBoards&) = delete;
    LobbyBoards& operator=(const LobbyBoards&) = delete;

    // The pages from what viewerconfig has, laid out. Once a frame while
    // they show, before render().
    void update();
    void render(SDL_GPUCommandBuffer* commands);
    void reload();

    gui::Surface& newsSurface() { return *news_; }
    gui::Surface& statusSurface() { return *status_; }
    // In a headset: links are said to open on the desktop.
    void setHeadset(bool headset);
    // A link clicked, to open in the web browser (once).
    std::optional<std::string> takeLink() { return std::exchange(link_, std::nullopt); }

    struct Entry
    {
        Rml::String title;
        Rml::String summary;
        Rml::String source;
        Rml::String when;
        Rml::String image; // image://... once it's here; empty until then (or none)
        bool pinned = false;
    };
    struct Notice // an announcement, incident or maintenance
    {
        Rml::String kind; // a style: info, warning, critical, incident, maintenance
        Rml::String title;
        Rml::String status;
        Rml::String body;
        Rml::String when;
        bool link = false; // clicking it opens its page
    };
    struct Component
    {
        Rml::String name;
        Rml::String group;
        Rml::String status; // in words
        Rml::String kind;   // a style: degraded, partial, major, maintenance
    };

private:
    LobbyBoards(gui::Gui& gui, net::HttpClient& http, JobSystem& jobs, const ViewerConfig& source);
    bool setUp();
    void fill();
    // The news entry opened (openId_) shown, or the list if it's gone.
    void fillOpened();
    // Scrolls the news page's content to `to`; where it was.
    float scrollTop(float to);
    std::string image(const std::string& url);
    void picturesArrived();

    gui::Gui& gui_;
    JobSystem& jobs_;
    const ViewerConfig& source_;
    std::unique_ptr<net::HttpRequestGroup> group_;
    std::unique_ptr<gui::Surface> news_;
    std::unique_ptr<gui::Surface> status_;
    Rml::DataModelHandle newsModel_;
    Rml::DataModelHandle statusModel_;
    std::optional<uint64_t> filled_; // the source's version shown
    std::chrono::steady_clock::time_point filledAt_{};

    struct Picture
    {
        std::string url;
        std::optional<gui::Pixels> pixels;
    };
    std::shared_ptr<MessageQueue<Picture>> pictures_ = std::make_shared<MessageQueue<Picture>>();
    // By address: the image's name once here, empty while coming, "-" if
    // it couldn't be had.
    std::map<std::string, std::string> images_;
    int nextImage_ = 0;

    std::optional<std::string> link_;
    bool headset_ = false;

    // The news page.
    Rml::String newsState_ = "loading"; // loading, empty, shown
    std::vector<Entry> entries_;
    std::vector<std::string> entryIds_; // entries_' ids
    // The entry shown whole (empty: the list).
    std::string openId_;
    bool opened_ = false;
    Rml::String openTitle_;
    Rml::String openSource_;
    Rml::String openWhen_;
    Rml::String openImage_;
    Rml::String openText_;
    std::string openUrl_;
    bool openLink_ = false;
    float listScroll_ = 0.0f;       // the list's, while an entry's open
    std::optional<float> scrollTo_; // after the next layout
    // The status page.
    Rml::String statusState_ = "loading"; // loading, shown
    Rml::String indicator_;               // none, minor, major, critical, maintenance
    Rml::String appName_{branding::kName};
    Rml::String description_;
    Rml::String checked_; // when Linden Lab's status was fetched
    bool stale_ = false;
    std::vector<Notice> announcements_;
    std::vector<Notice> incidents_;
    std::vector<std::string> incidentUrls_;
    std::string pageUrl_; // Linden Lab's status page
    bool hasPage_ = false;
    std::vector<Component> components_; // those not operational
};

} // namespace viewer
