#include "ViewerConfig.hpp"

#include "viewer/core/Branding.hpp"
#include "viewer/core/Sha256.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace viewer;
namespace fs = std::filesystem;

namespace
{

// Requests kept, answered when the test says.
class FakeHttp final : public net::HttpClient
{
public:
    struct Sent
    {
        net::HttpRequest request;
        net::HttpCallback callback;
    };
    net::HttpRequestId send(net::HttpRequest request, net::HttpCallback callback) override
    {
        sent.push_back({std::move(request), std::move(callback)});
        return sent.size();
    }
    bool cancel(net::HttpRequestId) override { return false; }

    void answer(size_t i, std::string_view body, int status = 200, std::string etag = {})
    {
        net::HttpResponse response;
        response.requestId = i + 1;
        response.status = status;
        response.body.assign(body.begin(), body.end());
        if(!etag.empty())
        {
            response.headers.add("etag", etag);
        }
        sent[i].callback(std::move(response));
    }
    std::vector<Sent> sent;
};

constexpr const char* kConfig = R"({
  "schema": 1,
  "latest_version": {"version": "0.10.0", "url": "https://example.org/download",
                     "notes": "New things.", "required": false},
  "flags": [
    {"name": "pbr_terrain", "enabled": true, "min_version": null, "max_version": "0.3.9",
     "platforms": ["linux", "windows"]},
    {"name": "pbr_terrain", "enabled": false, "min_version": null, "max_version": null,
     "platforms": null}
  ],
  "settings": [
    {"key": "ShadowDistance", "value": 96, "min_version": "0.4.0", "max_version": null,
     "platforms": null}
  ],
  "home_scene": null,
  "refresh": {"news": 30, "grid_status": 120}
})";

constexpr const char* kNews = R"({
  "schema": 1,
  "entries": [
    {"id": "post-12", "source": {"id": "viewer", "name": "TestViewer"},
     "title": "Welcome", "summary": "Hello.", "body": "Hello, world!", "url": null,
     "published": "2026-11-20T15:04:05Z",
     "image": {"url": "https://host/media/news/ab.png", "width": 640, "height": 360},
     "pinned": true},
    {"id": "feed-3-9f", "source": {"id": "sl-blog", "name": "Second Life Blog"},
     "title": "Viewer update", "summary": "It's here.", "body": null,
     "url": "https://community.secondlife.com/news/1", "published": "2026-11-18T17:00:00Z",
     "image": null, "pinned": false}
  ]
})";

constexpr const char* kGridStatus = R"({
  "schema": 1, "fetched": "2026-11-20T15:03:00Z", "stale": false,
  "page_url": "https://status.secondlifegrid.net/",
  "overall": {"indicator": "minor", "description": "Partially Degraded Service"},
  "announcements": [{"id": 4, "severity": "warning", "title": "Logins failing",
                     "body": "LL knows.", "posted": "2026-11-20T14:30:00Z"}],
  "components": [
    {"name": "Logins", "group": "Second Life Services", "status": "operational"},
    {"name": "Group Text Chat", "group": "Inworld Services", "status": "degraded_performance"}
  ],
  "incidents": [{"name": "Group Chat", "status": "investigating", "impact": "minor",
                 "update": "Looking.", "updated": "2026-11-20T13:10:00Z", "url": "https://x/1"}],
  "maintenance": []
})";

fs::path freshDirectory(const char* name)
{
    const fs::path dir = fs::path(VIEWER_TEST_TEMP_DIR) / name;
    fs::remove_all(dir);
    return dir;
}

} // namespace

TEST_CASE("viewerconfig: versions compare as numbers")
{
    CHECK(compareVersions("0.10.0", "0.9.2") > 0);
    CHECK(compareVersions("0.9.2", "0.10.0") < 0);
    CHECK(compareVersions("1.2", "1.2.0") == 0);
    CHECK(compareVersions("1.2.1", "1.2") > 0);
    CHECK(compareVersions("2.0.0-beta", "2.0.0") == 0);
}

TEST_CASE("viewerconfig: the config, and what applies to us")
{
    const auto config = parseClientConfig(kConfig);
    REQUIRE(config);
    REQUIRE(config->latest);
    CHECK(config->latest->version == "0.10.0");
    REQUIRE(config->flags.size() == 2);
    CHECK(config->flags[0].applies("0.3.0", "linux"));
    CHECK_FALSE(config->flags[0].applies("0.3.0", "macos"));
    CHECK_FALSE(config->flags[0].applies("0.4.0", "linux"));
    CHECK(config->flags[1].applies("9.0", "macos"));
    REQUIRE(config->settings.size() == 1);
    CHECK(config->settings[0].value.asInteger() == 96);
    CHECK_FALSE(config->homeScene);
    CHECK(config->newsRefresh == 30);

    CHECK_FALSE(parseClientConfig(R"({"schema": 2})"));
    CHECK_FALSE(parseClientConfig("<html>"));
    CHECK_FALSE(parseClientConfig(
        R"({"schema": 1, "home_scene": {"url": "https://x/s", "sha256": "nothex", "size": 1}})"));
}

TEST_CASE("viewerconfig: news and grid status")
{
    const auto news = parseNews(kNews);
    REQUIRE(news);
    REQUIRE(news->size() == 2);
    CHECK((*news)[0].pinned);
    CHECK((*news)[0].imageWidth == 640);
    CHECK((*news)[0].url.empty());
    CHECK((*news)[1].sourceName == "Second Life Blog");
    CHECK((*news)[1].body.empty());
    CHECK((*news)[1].imageUrl.empty());

    const auto status = parseGridStatus(kGridStatus);
    REQUIRE(status);
    CHECK(status->indicator == "minor");
    CHECK(status->components.size() == 2);
    REQUIRE(status->announcements.size() == 1);
    CHECK(status->announcements[0].severity == "warning");
    CHECK(status->incidents.size() == 1);
    CHECK(status->maintenance.empty());
}

TEST_CASE("viewerconfig: asks, keeps copies, and asks again with their ETags")
{
    const fs::path dir = freshDirectory("viewerconfig-copies");
    JobSystem jobs(1);
    const auto t0 = ViewerConfig::Clock::time_point{} + std::chrono::hours(1);
    {
        FakeHttp http;
        ViewerConfig vc(http, jobs, {"https://vc.example/api/v1", dir, "0.4.0", "linux"});
        vc.update(t0, false);
        // The config at once; nothing else outside the lobby.
        REQUIRE(http.sent.size() == 1);
        CHECK(http.sent[0].request.url == "https://vc.example/api/v1/config");
        CHECK(http.sent[0].request.headers.find("User-Agent") ==
              std::string(branding::kUserAgent) + "/0.4.0 (linux)");
        CHECK_FALSE(http.sent[0].request.headers.find("If-None-Match"));
        http.answer(0, kConfig, 200, "\"c1\"");
        vc.update(t0, false);
        CHECK(vc.flag("pbr_terrain", true) == false); // the first rule's for older ones
        REQUIRE(vc.setting("ShadowDistance"));
        CHECK_FALSE(vc.setting("Unknown"));
        REQUIRE(vc.newerRelease());

        // The lobby: news and status.
        vc.update(t0, true);
        REQUIRE(http.sent.size() == 3);
        http.answer(1, kNews, 200, "\"n1\"");
        http.answer(2, kGridStatus, 200, "\"g1\"");
        vc.update(t0, true);
        CHECK(vc.news().size() == 2);
        CHECK(vc.newsKnown());
        CHECK(vc.gridStatus());
        // Again only after the refresh (news: not below its minimum).
        vc.update(t0 + std::chrono::seconds(100), true);
        CHECK(http.sent.size() == 3);
        vc.update(t0 + std::chrono::seconds(121), true);
        REQUIRE(http.sent.size() == 4);
        CHECK(http.sent[3].request.url.ends_with("/grid-status"));
        CHECK(http.sent[3].request.headers.find("If-None-Match") == "\"g1\"");
        http.answer(3, "", 304);
        vc.update(t0 + std::chrono::seconds(121), true);
        CHECK(vc.gridStatus()); // still the copy
        // The news after its minimum (300 s, not the 30 asked for); the
        // status again too by then.
        vc.update(t0 + ViewerConfig::kMinNewsRefresh - std::chrono::seconds(1), true);
        CHECK(http.sent.size() == 5);
        CHECK(http.sent[4].request.url.ends_with("/grid-status"));
        vc.update(t0 + ViewerConfig::kMinNewsRefresh, true);
        REQUIRE(http.sent.size() == 6);
        CHECK(http.sent[5].request.url.ends_with("/news"));
    }
    {
        // Next time: the copies at once, and their ETags sent.
        FakeHttp http;
        ViewerConfig vc(http, jobs, {"https://vc.example/api/v1/", dir, "0.4.0", "linux"});
        CHECK(vc.news().size() == 2);
        CHECK(vc.gridStatus());
        vc.update(t0, false);
        REQUIRE(http.sent.size() == 1);
        CHECK(http.sent[0].request.headers.find("If-None-Match") == "\"c1\"");
        // The service down: the copies stay.
        http.answer(0, "", 503);
        vc.update(t0, false);
        CHECK(vc.flag("pbr_terrain", true) == false);
        // Asked again later.
        vc.update(t0 + ViewerConfig::kConfigRetry, false);
        CHECK(http.sent.size() == 2);
    }
}

TEST_CASE("viewerconfig: the home scene, downloaded once and checked")
{
    const fs::path dir = freshDirectory("viewerconfig-scene");
    JobSystem jobs(1);
    const std::string scene = "a scene's bytes";
    const std::string sha =
        sha256Hex(std::span(reinterpret_cast<const uint8_t*>(scene.data()), scene.size()));
    const std::string config = std::format(
        R"({{"schema": 1, "home_scene": {{"url": "https://vc.example/s/{0}.scene",
            "sha256": "{0}", "size": {1}}}}})",
        sha, scene.size());
    const auto now = ViewerConfig::Clock::time_point{};
    auto waitFor = [&](ViewerConfig& vc, auto done)
    {
        for(int i = 0; i < 500 && !done(); i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            vc.update(now, false);
        }
    };

    FakeHttp http;
    ViewerConfig vc(http, jobs, {"https://vc.example/api/v1/", dir, "0.4.0", "linux"});
    vc.update(now, false);
    http.answer(0, config, 200, "\"c\"");
    vc.update(now, false);
    REQUIRE(http.sent.size() == 2);
    CHECK(http.sent[1].request.url == std::format("https://vc.example/s/{}.scene", sha));
    // Not what was announced: not kept.
    http.answer(1, "something else");
    waitFor(vc, [&] { return false; });
    CHECK(vc.homeScene().empty());

    // The next config asks again; the right one's kept.
    FakeHttp http2;
    ViewerConfig vc2(http2, jobs, {"https://vc.example/api/v1/", dir, "0.4.0", "linux"});
    REQUIRE(http2.sent.size() == 1); // the copy's scene, asked for at once
    http2.answer(0, scene);
    waitFor(vc2, [&] { return !vc2.homeScene().empty(); });
    REQUIRE_FALSE(vc2.homeScene().empty());
    CHECK(vc2.homeScene().filename() == sha + ".scene");
    CHECK(fs::file_size(vc2.homeScene()) == scene.size());

    // On disk: not downloaded again.
    FakeHttp http3;
    ViewerConfig vc3(http3, jobs, {"https://vc.example/api/v1/", dir, "0.4.0", "linux"});
    CHECK(http3.sent.empty());
    CHECK(vc3.homeScene() == vc2.homeScene());
}
