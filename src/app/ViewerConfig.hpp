#pragma once

// viewerconfig: the project's own web service (docs/viewerconfig-api.md),
// asked instead of Linden Lab's sites. Its config (feature flags, overrides
// of settings' defaults, the newest release, the lobby's home scene) once at
// startup; its news and grid status when the lobby shows, and again every so
// often while it does. Every answer's last good copy is kept on disk (with
// its ETag, sent back so an unchanged answer is a 304), and shown until a
// newer one comes, so the lobby has something even when the service is down.
//
// The home scene is downloaded only when the config names one not on disk
// yet, and kept only if its size and SHA-256 are what the config said.
//
// Nothing about the viewer or its user is sent: the requests are the same
// for everyone but the User-Agent ("TestViewer/<version> (<os>)"). Flags
// and settings come with the versions and platforms they're for, and are
// picked here.

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

struct ViewerRelease
{
    std::string version;
    std::string url;
    std::string notes;
    bool required = false; // older viewers mustn't log in
};

// A flag's value or a setting's, for some versions and platforms.
struct ConfigRule
{
    std::string name;
    llsd::Value value;
    std::string minVersion; // inclusive; empty: any
    std::string maxVersion;
    std::optional<std::vector<std::string>> platforms; // nothing: all
    bool applies(std::string_view version, std::string_view platform) const;
};

struct HomeSceneRef
{
    std::string url;
    std::string sha256; // lowercase hex
    uint64_t size = 0;
};

struct ClientConfig
{
    std::optional<ViewerRelease> latest;
    std::vector<ConfigRule> flags;
    std::vector<ConfigRule> settings;
    std::optional<HomeSceneRef> homeScene; // nothing: the built-in one
    int newsRefresh = 900;                 // seconds
    int gridStatusRefresh = 120;
};

struct NewsEntry
{
    std::string id;
    std::string sourceId;
    std::string sourceName;
    std::string title;
    std::string summary;
    std::string body; // a post's whole text; empty for a feed's entries
    std::string url;  // what clicking it opens; empty: nothing
    std::string published;
    std::string imageUrl;
    int imageWidth = 0;
    int imageHeight = 0;
    bool pinned = false;
};

struct GridStatus
{
    std::string fetched;
    bool stale = false;
    std::string pageUrl;
    std::string indicator; // none, minor, major, critical, maintenance
    std::string description;
    struct Announcement
    {
        std::string severity; // info, warning, critical
        std::string title;
        std::string body;
        std::string posted;
    };
    std::vector<Announcement> announcements;
    struct Component
    {
        std::string name;
        std::string group;
        std::string status; // operational, degraded_performance, ...
    };
    std::vector<Component> components;
    struct Incident
    {
        std::string name;
        std::string status;
        std::string impact;
        std::string update;
        std::string updated;
        std::string url;
    };
    std::vector<Incident> incidents;
    struct Maintenance
    {
        std::string name;
        std::string status;
        std::string starts;
        std::string ends;
        std::string update;
        std::string url;
    };
    std::vector<Maintenance> maintenance;
};

// Dot-separated numbers: below 0 if a is older, 0 if the same, above if
// newer ("0.10.0" is newer than "0.9.2"; missing parts are 0).
int compareVersions(std::string_view a, std::string_view b);

std::expected<ClientConfig, std::string> parseClientConfig(std::string_view json);
std::expected<std::vector<NewsEntry>, std::string> parseNews(std::string_view json);
std::expected<GridStatus, std::string> parseGridStatus(std::string_view json);

// "linux", "windows" or "macos".
std::string_view platformName();

class ViewerConfig
{
public:
    using Clock = std::chrono::steady_clock;
    // The service's own refresh times aren't taken below these.
    static constexpr auto kMinNewsRefresh = std::chrono::seconds(300);
    static constexpr auto kMinGridStatusRefresh = std::chrono::seconds(60);
    // The config asked again after a failure.
    static constexpr auto kConfigRetry = std::chrono::seconds(300);

    struct Options
    {
        std::string baseUrl; // ".../api/v1/"; empty: never asked (the disk's copies only)
        std::filesystem::path directory; // where the copies are kept
        std::string version;             // ours
        std::string platform;            // ours (platformName())
    };
    ViewerConfig(net::HttpClient& http, JobSystem& jobs, Options options);
    ~ViewerConfig();

    ViewerConfig(const ViewerConfig&) = delete;
    ViewerConfig& operator=(const ViewerConfig&) = delete;

    // What came; asks for what's due (news and grid status only while the
    // lobby shows). Once a frame.
    void update(Clock::time_point now, bool lobby);

    const ClientConfig& config() const { return config_; }
    // A flag's value for us (`fallback`: no rule for us).
    bool flag(std::string_view name, bool fallback) const;
    // A setting's default for us; null if it keeps the viewer's own.
    const llsd::Value* setting(std::string_view key) const;
    // The newest release, if newer than us.
    std::optional<ViewerRelease> newerRelease() const;

    const std::vector<NewsEntry>& news() const { return news_; }
    // The news has come (or was on disk): none is no news, not none yet.
    bool newsKnown() const { return newsKnown_; }
    const std::optional<GridStatus>& gridStatus() const { return gridStatus_; }
    // Changes whenever the news or grid status do.
    uint64_t version() const { return version_; }

    // The home scene's file; empty: the built-in one. Changes with
    // homeSceneVersion().
    const std::filesystem::path& homeScene() const { return homeScene_; }
    uint64_t homeSceneVersion() const { return homeSceneVersion_; }

private:
    enum class Kind
    {
        Config,
        News,
        GridStatus,
        HomeScene,
    };
    struct Answer
    {
        Kind kind = Kind::Config;
        net::HttpResponse response;
        std::string scene; // HomeScene: the file written (empty: no good)
    };
    struct Endpoint
    {
        Kind kind = Kind::Config;
        std::string name; // its path under the base URL, and its files' names
        std::string etag;
        bool asking = false;
        std::optional<Clock::time_point> due; // when to ask next (nothing: not now)
    };

    void loadCopies();
    void ask(Endpoint& e);
    void answered(Answer& a, Clock::time_point now);
    // The body is good (it parsed): kept on disk with its ETag.
    void keep(Endpoint& e, const net::HttpResponse& response);
    bool use(Kind kind, std::string_view json);
    void wantHomeScene();
    Endpoint& endpoint(Kind kind);
    std::chrono::seconds refresh(Kind kind) const;

    net::HttpClient& http_;
    JobSystem& jobs_;
    Options options_;
    std::unique_ptr<net::HttpRequestGroup> group_;
    // Shared with the requests' and workers' jobs, which may finish after
    // we've gone.
    std::shared_ptr<MessageQueue<Answer>> answers_ = std::make_shared<MessageQueue<Answer>>();
    std::vector<Endpoint> endpoints_;
    bool lobby_ = false;

    ClientConfig config_;
    std::vector<NewsEntry> news_;
    bool newsKnown_ = false;
    std::optional<GridStatus> gridStatus_;
    uint64_t version_ = 0;

    std::filesystem::path homeScene_;
    uint64_t homeSceneVersion_ = 0;
    std::string sceneComing_; // the hash being downloaded
};

} // namespace viewer
