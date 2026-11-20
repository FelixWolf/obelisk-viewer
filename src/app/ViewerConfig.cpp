#include "ViewerConfig.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/core/Sha256.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace viewer
{

namespace
{

constexpr int kSchema = 1;

std::string text(const llsd::Value& v)
{
    return v.isUndefined() ? std::string() : v.asString();
}

// The answer as a map of this schema, or why not.
std::expected<llsd::Value, std::string> readAnswer(std::string_view json)
{
    auto root = llsd::fromJson(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    if(!root->isMap())
    {
        return std::unexpected("not an object");
    }
    if((*root)["schema"].asInteger() != kSchema)
    {
        return std::unexpected(std::format("schema {}, not {}", text((*root)["schema"]), kSchema));
    }
    return std::move(*root);
}

ConfigRule rule(const llsd::Value& v, const char* nameKey, const char* valueKey)
{
    ConfigRule r;
    r.name = text(v[nameKey]);
    r.value = v[valueKey];
    r.minVersion = text(v["min_version"]);
    r.maxVersion = text(v["max_version"]);
    const llsd::Value& platforms = v["platforms"];
    if(platforms.isArray())
    {
        r.platforms.emplace();
        for(size_t i = 0; i < platforms.size(); i++)
        {
            r.platforms->push_back(platforms.at(i).asString());
        }
    }
    return r;
}

std::optional<std::string> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Written whole or not at all (another file, then renamed over it).
bool writeFile(const fs::path& path, std::string_view contents)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path partial = path;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if(!out.write(contents.data(), static_cast<std::streamsize>(contents.size())))
        {
            return false;
        }
    }
    fs::rename(partial, path, ec);
    return !ec;
}

fs::path scenePath(const fs::path& directory, std::string_view sha)
{
    return directory / "scenes" / (std::string(sha) + ".scene");
}

bool isHex(std::string_view s)
{
    return std::ranges::all_of(s, [](char c)
                               { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& s)
    {
        int n = 0;
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        s.remove_prefix(static_cast<size_t>(end - s.data()));
        // Whatever's after the number (a suffix), then the dot.
        const size_t dot = s.find('.');
        s.remove_prefix(dot == std::string_view::npos ? s.size() : dot + 1);
        return ec == std::errc() ? n : 0;
    };
    while(!a.empty() || !b.empty())
    {
        const int x = next(a);
        const int y = next(b);
        if(x != y)
        {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

bool ConfigRule::applies(std::string_view version, std::string_view platform) const
{
    if(!minVersion.empty() && compareVersions(version, minVersion) < 0)
    {
        return false;
    }
    if(!maxVersion.empty() && compareVersions(version, maxVersion) > 0)
    {
        return false;
    }
    return !platforms || std::ranges::find(*platforms, platform) != platforms->end();
}

std::expected<ClientConfig, std::string> parseClientConfig(std::string_view json)
{
    auto root = readAnswer(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    const llsd::Value& r = *root;
    ClientConfig c;
    if(const llsd::Value& latest = r["latest_version"]; latest.isMap())
    {
        c.latest = ViewerRelease{text(latest["version"]), text(latest["url"]),
                                 text(latest["notes"]), latest["required"].asBoolean()};
    }
    for(size_t i = 0; i < r["flags"].size(); i++)
    {
        ConfigRule flag = rule(r["flags"].at(i), "name", "enabled");
        if(!flag.name.empty() && flag.value.type() == llsd::Value::Type::Boolean)
        {
            c.flags.push_back(std::move(flag));
        }
    }
    for(size_t i = 0; i < r["settings"].size(); i++)
    {
        ConfigRule setting = rule(r["settings"].at(i), "key", "value");
        if(!setting.name.empty() && !setting.value.isUndefined())
        {
            c.settings.push_back(std::move(setting));
        }
    }
    if(const llsd::Value& scene = r["home_scene"]; scene.isMap())
    {
        HomeSceneRef ref{text(scene["url"]), text(scene["sha256"]),
                         static_cast<uint64_t>(std::max(scene["size"].asReal(), 0.0))};
        std::ranges::transform(ref.sha256, ref.sha256.begin(), [](unsigned char ch)
                               { return static_cast<char>(std::tolower(ch)); });
        if(ref.url.empty() || ref.sha256.size() != 64 || !isHex(ref.sha256))
        {
            return std::unexpected("a home scene without its address or hash");
        }
        c.homeScene = std::move(ref);
    }
    const llsd::Value& refresh = r["refresh"];
    if(refresh["news"].asInteger() > 0)
    {
        c.newsRefresh = refresh["news"].asInteger();
    }
    if(refresh["grid_status"].asInteger() > 0)
    {
        c.gridStatusRefresh = refresh["grid_status"].asInteger();
    }
    return c;
}

std::expected<std::vector<NewsEntry>, std::string> parseNews(std::string_view json)
{
    auto root = readAnswer(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    const llsd::Value& entries = (*root)["entries"];
    if(!entries.isArray())
    {
        return std::unexpected("no entries");
    }
    std::vector<NewsEntry> news;
    for(size_t i = 0; i < entries.size(); i++)
    {
        const llsd::Value& e = entries.at(i);
        NewsEntry n;
        n.id = text(e["id"]);
        n.sourceId = text(e["source"]["id"]);
        n.sourceName = text(e["source"]["name"]);
        n.title = text(e["title"]);
        n.summary = text(e["summary"]);
        n.body = text(e["body"]);
        n.url = text(e["url"]);
        n.published = text(e["published"]);
        n.imageUrl = text(e["image"]["url"]);
        n.imageWidth = e["image"]["width"].asInteger();
        n.imageHeight = e["image"]["height"].asInteger();
        n.pinned = e["pinned"].asBoolean();
        if(!n.id.empty() && !n.title.empty())
        {
            news.push_back(std::move(n));
        }
    }
    return news;
}

std::expected<GridStatus, std::string> parseGridStatus(std::string_view json)
{
    auto root = readAnswer(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    const llsd::Value& r = *root;
    GridStatus g;
    g.fetched = text(r["fetched"]);
    g.stale = r["stale"].asBoolean();
    g.pageUrl = text(r["page_url"]);
    g.indicator = text(r["overall"]["indicator"]);
    g.description = text(r["overall"]["description"]);
    for(size_t i = 0; i < r["announcements"].size(); i++)
    {
        const llsd::Value& a = r["announcements"].at(i);
        g.announcements.push_back(
            {text(a["severity"]), text(a["title"]), text(a["body"]), text(a["posted"])});
    }
    for(size_t i = 0; i < r["components"].size(); i++)
    {
        const llsd::Value& c = r["components"].at(i);
        g.components.push_back({text(c["name"]), text(c["group"]), text(c["status"])});
    }
    for(size_t i = 0; i < r["incidents"].size(); i++)
    {
        const llsd::Value& n = r["incidents"].at(i);
        g.incidents.push_back({text(n["name"]), text(n["status"]), text(n["impact"]),
                               text(n["update"]), text(n["updated"]), text(n["url"])});
    }
    for(size_t i = 0; i < r["maintenance"].size(); i++)
    {
        const llsd::Value& m = r["maintenance"].at(i);
        g.maintenance.push_back({text(m["name"]), text(m["status"]), text(m["starts"]),
                                 text(m["ends"]), text(m["update"]), text(m["url"])});
    }
    return g;
}

std::string_view platformName()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

ViewerConfig::ViewerConfig(net::HttpClient& http, JobSystem& jobs, Options options)
    : http_(http), jobs_(jobs), options_(std::move(options)),
      group_(std::make_unique<net::HttpRequestGroup>(http))
{
    endpoints_ = {
        {Kind::Config, "config", {}, false, std::nullopt},
        {Kind::News, "news", {}, false, std::nullopt},
        {Kind::GridStatus, "grid-status", {}, false, std::nullopt},
    };
    if(!options_.baseUrl.empty() && options_.baseUrl.back() != '/')
    {
        options_.baseUrl += '/';
    }
    loadCopies();
    // The config at once (the rest when the lobby shows).
    endpoint(Kind::Config).due = Clock::time_point{};
}

ViewerConfig::~ViewerConfig()
{
    group_.reset(); // no answers after this
}

ViewerConfig::Endpoint& ViewerConfig::endpoint(Kind kind)
{
    return *std::ranges::find(endpoints_, kind, &Endpoint::kind);
}

void ViewerConfig::loadCopies()
{
    for(Endpoint& e : endpoints_)
    {
        const auto json = readFile(options_.directory / (e.name + ".json"));
        if(json && use(e.kind, *json))
        {
            e.etag = readFile(options_.directory / (e.name + ".etag")).value_or("");
        }
    }
}

bool ViewerConfig::use(Kind kind, std::string_view json)
{
    switch(kind)
    {
    case Kind::Config:
        if(auto c = parseClientConfig(json))
        {
            config_ = std::move(*c);
            wantHomeScene();
            return true;
        }
        else
        {
            log::warn("viewerconfig: config: {}", c.error());
        }
        break;
    case Kind::News:
        if(auto n = parseNews(json))
        {
            news_ = std::move(*n);
            newsKnown_ = true;
            version_++;
            return true;
        }
        else
        {
            log::warn("viewerconfig: news: {}", n.error());
        }
        break;
    case Kind::GridStatus:
        if(auto g = parseGridStatus(json))
        {
            gridStatus_ = std::move(*g);
            version_++;
            return true;
        }
        else
        {
            log::warn("viewerconfig: grid status: {}", g.error());
        }
        break;
    case Kind::HomeScene:
        break;
    }
    return false;
}

void ViewerConfig::wantHomeScene()
{
    if(!config_.homeScene)
    {
        if(!homeScene_.empty())
        {
            homeScene_.clear();
            homeSceneVersion_++;
        }
        return;
    }
    const HomeSceneRef& ref = *config_.homeScene;
    const fs::path path = scenePath(options_.directory, ref.sha256);
    std::error_code ec;
    if(fs::exists(path, ec))
    {
        if(homeScene_ != path)
        {
            homeScene_ = path;
            homeSceneVersion_++;
        }
        return;
    }
    if(sceneComing_ == ref.sha256 || options_.baseUrl.empty())
    {
        return;
    }
    sceneComing_ = ref.sha256;
    log::info("viewerconfig: downloading home scene {}", ref.sha256);
    net::HttpRequest request;
    request.url = ref.url;
    request.headers.add("User-Agent",
                        std::format("TestViewer/{} ({})", options_.version, options_.platform));
    request.timeout = std::chrono::minutes(5);
    auto answers = answers_;
    JobSystem* jobs = &jobs_;
    group_->send(std::move(request),
                 [answers, jobs, ref, path](net::HttpResponse&& response)
                 {
                     if(!response.ok())
                     {
                         answers->push({Kind::HomeScene, std::move(response), {}});
                         return;
                     }
                     // Checked and written on a worker (it may be big).
                     jobs->submit(
                         [answers, ref, path, response = std::move(response)]() mutable
                         {
                             std::string written;
                             const bool good = response.body.size() == ref.size &&
                                               sha256Hex(response.body) == ref.sha256;
                             if(good && writeFile(path, response.bodyText()))
                             {
                                 written = path.string();
                             }
                             response.body.clear();
                             answers->push({Kind::HomeScene, std::move(response), written});
                         });
                 });
}

void ViewerConfig::ask(Endpoint& e)
{
    e.asking = true;
    e.due.reset();
    net::HttpRequest request;
    request.url = options_.baseUrl + e.name;
    request.headers.add("Accept", "application/json");
    request.headers.add("User-Agent",
                        std::format("TestViewer/{} ({})", options_.version, options_.platform));
    if(!e.etag.empty())
    {
        request.headers.add("If-None-Match", e.etag);
    }
    auto answers = answers_;
    const Kind kind = e.kind;
    group_->send(std::move(request), [answers, kind](net::HttpResponse&& response)
                 { answers->push({kind, std::move(response), {}}); });
}

void ViewerConfig::keep(Endpoint& e, const net::HttpResponse& response)
{
    e.etag = std::string(response.headers.find("etag").value_or(""));
    writeFile(options_.directory / (e.name + ".json"), response.bodyText());
    writeFile(options_.directory / (e.name + ".etag"), e.etag);
}

std::chrono::seconds ViewerConfig::refresh(Kind kind) const
{
    switch(kind)
    {
    case Kind::News:
        return std::max(std::chrono::seconds(config_.newsRefresh), kMinNewsRefresh);
    case Kind::GridStatus:
        return std::max(std::chrono::seconds(config_.gridStatusRefresh), kMinGridStatusRefresh);
    default:
        return kConfigRetry;
    }
}

void ViewerConfig::answered(Answer& a, Clock::time_point now)
{
    const net::HttpResponse& r = a.response;
    if(a.kind == Kind::HomeScene)
    {
        const std::string sha = std::exchange(sceneComing_, {});
        if(a.scene.empty())
        {
            // Asked again with the next config.
            log::warn("viewerconfig: home scene {} not had ({})", sha,
                      r.ok()                            ? std::string("not what was announced")
                      : r.error != net::HttpError::None ? std::string(net::errorName(r.error))
                                                        : std::format("{}", r.status));
            return;
        }
        log::info("viewerconfig: home scene {} downloaded", sha);
        // Only the scene wanted now is kept.
        std::error_code ec;
        for(const auto& f : fs::directory_iterator(options_.directory / "scenes", ec))
        {
            if(f.path() != fs::path(a.scene) && !f.path().string().ends_with(".partial"))
            {
                fs::remove(f.path(), ec);
            }
        }
        wantHomeScene();
        return;
    }

    Endpoint& e = endpoint(a.kind);
    e.asking = false;
    // The config once (unless it failed); the others again while the
    // lobby shows.
    if(a.kind != Kind::Config || !(r.ok() || r.status == 304))
    {
        e.due = now + refresh(a.kind);
    }
    if(r.status == 304)
    {
        return;
    }
    if(!r.ok())
    {
        log::warn("viewerconfig: {}: {}", e.name,
                  r.error != net::HttpError::None
                      ? std::format("{} ({})", net::errorName(r.error), r.errorMessage)
                      : std::format("answered {}", r.status));
        return;
    }
    if(use(a.kind, r.bodyText()))
    {
        keep(e, r);
    }
}

void ViewerConfig::update(Clock::time_point now, bool lobby)
{
    std::vector<Answer> answers;
    answers_->drain(answers);
    for(Answer& a : answers)
    {
        answered(a, now);
    }

    // The news and status when the lobby shows (an unchanged answer costs
    // a 304).
    if(lobby && !lobby_)
    {
        endpoint(Kind::News).due = now;
        endpoint(Kind::GridStatus).due = now;
    }
    lobby_ = lobby;
    if(options_.baseUrl.empty())
    {
        return;
    }
    for(Endpoint& e : endpoints_)
    {
        const bool wanted = e.kind == Kind::Config || lobby;
        if(wanted && !e.asking && e.due && now >= *e.due)
        {
            ask(e);
        }
    }
}

bool ViewerConfig::flag(std::string_view name, bool fallback) const
{
    for(const ConfigRule& r : config_.flags)
    {
        if(r.name == name && r.applies(options_.version, options_.platform))
        {
            return r.value.asBoolean();
        }
    }
    return fallback;
}

const llsd::Value* ViewerConfig::setting(std::string_view key) const
{
    for(const ConfigRule& r : config_.settings)
    {
        if(r.name == key && r.applies(options_.version, options_.platform))
        {
            return &r.value;
        }
    }
    return nullptr;
}

std::optional<ViewerRelease> ViewerConfig::newerRelease() const
{
    if(config_.latest && compareVersions(config_.latest->version, options_.version) > 0)
    {
        return config_.latest;
    }
    return std::nullopt;
}

} // namespace viewer
