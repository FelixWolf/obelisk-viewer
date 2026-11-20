#pragma once

// The lobby's boards: the news and the grid's status (from viewerconfig),
// each an interface page (assets/ui/news.rml, grid_status.rml) on a panel
// standing where the home scene's markers say. They're only looked at for
// now: nothing on them can be pointed at or clicked.
//
// The news' pictures come from the web, decoded on the workers and given
// to the pages as image://news-N.

#include "ViewerConfig.hpp"

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

    // On panels at these places (centers), facing these points.
    void addTo(render::Scene& scene, const glm::mat4& newsFrame,
               const glm::mat4& statusFrame) const;

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

    // The news page.
    Rml::String newsState_ = "loading"; // loading, empty, shown
    std::vector<Entry> entries_;
    // The status page.
    Rml::String statusState_ = "loading"; // loading, shown
    Rml::String indicator_;               // none, minor, major, critical, maintenance
    Rml::String description_;
    Rml::String checked_; // when Linden Lab's status was fetched
    bool stale_ = false;
    std::vector<Notice> announcements_;
    std::vector<Notice> incidents_;
    std::vector<Component> components_; // those not operational
};

} // namespace viewer
