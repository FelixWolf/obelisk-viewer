#include "LobbyBoards.hpp"

#include "viewer/core/Log.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace viewer
{

namespace
{

constexpr const char* kFailed = "-";
// The times shown ("5 minutes ago") are made again this often.
constexpr auto kRetell = std::chrono::seconds(30);

std::optional<std::chrono::sys_seconds> parseTime(std::string_view iso)
{
    int y = 0;
    unsigned mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const std::string text(iso);
    if(std::sscanf(text.c_str(), "%d-%u-%uT%u:%u:%u", &y, &mo, &d, &h, &mi, &s) != 6)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{std::chrono::year(y), std::chrono::month(mo),
                                          std::chrono::day(d)};
    if(!day.ok())
    {
        return std::nullopt;
    }
    return std::chrono::sys_days(day) + std::chrono::hours(h) + std::chrono::minutes(mi) +
           std::chrono::seconds(s);
}

std::string_view componentStatus(std::string_view status)
{
    if(status == "degraded_performance")
    {
        return "Degraded";
    }
    if(status == "partial_outage")
    {
        return "Partial outage";
    }
    if(status == "major_outage")
    {
        return "Major outage";
    }
    if(status == "under_maintenance")
    {
        return "Maintenance";
    }
    return status;
}

std::string_view componentKind(std::string_view status)
{
    if(status == "degraded_performance")
    {
        return "degraded";
    }
    if(status == "partial_outage")
    {
        return "partial";
    }
    if(status == "major_outage")
    {
        return "major";
    }
    return "maintenance";
}

// "investigating" -> "Investigating".
std::string capitalized(std::string_view s)
{
    std::string out(s);
    std::ranges::replace(out, '_', ' ');
    if(!out.empty())
    {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

} // namespace

std::string describeTime(std::string_view iso, std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;
    const auto t = parseTime(iso);
    if(!t)
    {
        return {};
    }
    const auto ago = duration_cast<seconds>(now - *t);
    auto plural = [](long n, std::string_view unit)
    { return std::format("{} {}{} ago", n, unit, n == 1 ? "" : "s"); };
    if(ago < minutes(1))
    {
        return "just now"; // or a little ahead (clocks differ)
    }
    if(ago < hours(1))
    {
        return plural(duration_cast<minutes>(ago).count(), "minute");
    }
    if(ago < days(1))
    {
        return plural(duration_cast<hours>(ago).count(), "hour");
    }
    if(ago < days(7))
    {
        return plural(duration_cast<days>(ago).count(), "day");
    }
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const year_month_day day{floor<days>(*t)};
    const year_month_day today{floor<days>(now)};
    const std::string date =
        std::format("{} {}", kMonths[unsigned(day.month()) - 1], unsigned(day.day()));
    return day.year() == today.year() ? date : std::format("{} {}", date, int(day.year()));
}

LobbyBoards::LobbyBoards(gui::Gui& gui, net::HttpClient& http, JobSystem& jobs,
                         const ViewerConfig& source)
    : gui_(gui), jobs_(jobs), source_(source), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

LobbyBoards::~LobbyBoards()
{
    group_.reset(); // no answers after this
    for(const auto& [url, name] : images_)
    {
        if(!name.empty() && name != kFailed)
        {
            gui_.removeImage(name);
        }
    }
}

std::unique_ptr<LobbyBoards> LobbyBoards::create(gui::Gui& gui, net::HttpClient& http,
                                                 JobSystem& jobs, const ViewerConfig& source)
{
    std::unique_ptr<LobbyBoards> b(new LobbyBoards(gui, http, jobs, source));
    if(!b->setUp())
    {
        return nullptr;
    }
    return b;
}

bool LobbyBoards::setUp()
{
    news_ = gui_.createSurface("news", kSize);
    status_ = gui_.createSurface("grid_status", kSize);
    if(!news_ || !status_)
    {
        return false;
    }

    Rml::DataModelConstructor news = news_->context()->CreateDataModel("news");
    if(!news)
    {
        return false;
    }
    if(auto entry = news.RegisterStruct<Entry>())
    {
        entry.RegisterMember("title", &Entry::title);
        entry.RegisterMember("summary", &Entry::summary);
        entry.RegisterMember("source", &Entry::source);
        entry.RegisterMember("when", &Entry::when);
        entry.RegisterMember("image", &Entry::image);
        entry.RegisterMember("pinned", &Entry::pinned);
    }
    news.RegisterArray<std::vector<Entry>>();
    news.Bind("state", &newsState_);
    news.Bind("entries", &entries_);
    news.Bind("opened", &opened_);
    news.Bind("openTitle", &openTitle_);
    news.Bind("openSource", &openSource_);
    news.Bind("openWhen", &openWhen_);
    news.Bind("openImage", &openImage_);
    news.Bind("openText", &openText_);
    news.Bind("openLink", &openLink_);
    news.Bind("headset", &headset_);
    news.BindEventCallback("show",
                           [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                           {
                               const int i = a.empty() ? -1 : a[0].Get<int>(-1);
                               if(i >= 0 && static_cast<size_t>(i) < entryIds_.size())
                               {
                                   listScroll_ = scrollTop(0.0f);
                                   openId_ = entryIds_[static_cast<size_t>(i)];
                                   fillOpened();
                                   scrollTo_ = 0.0f;
                               }
                           });
    news.BindEventCallback("back",
                           [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                           {
                               openId_.clear();
                               fillOpened();
                               scrollTo_ = listScroll_; // once it's laid out
                           });
    news.BindEventCallback("browse",
                           [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                           {
                               if(!openUrl_.empty())
                               {
                                   link_ = openUrl_;
                               }
                           });
    newsModel_ = news.GetModelHandle();

    Rml::DataModelConstructor status = status_->context()->CreateDataModel("status");
    if(!status)
    {
        return false;
    }
    if(auto notice = status.RegisterStruct<Notice>())
    {
        notice.RegisterMember("kind", &Notice::kind);
        notice.RegisterMember("title", &Notice::title);
        notice.RegisterMember("status", &Notice::status);
        notice.RegisterMember("body", &Notice::body);
        notice.RegisterMember("when", &Notice::when);
        notice.RegisterMember("link", &Notice::link);
    }
    status.RegisterArray<std::vector<Notice>>();
    if(auto component = status.RegisterStruct<Component>())
    {
        component.RegisterMember("name", &Component::name);
        component.RegisterMember("group", &Component::group);
        component.RegisterMember("status", &Component::status);
        component.RegisterMember("kind", &Component::kind);
    }
    status.RegisterArray<std::vector<Component>>();
    status.Bind("state", &statusState_);
    status.Bind("indicator", &indicator_);
    status.Bind("appName", &appName_);
    status.Bind("description", &description_);
    status.Bind("checked", &checked_);
    status.Bind("stale", &stale_);
    status.Bind("announcements", &announcements_);
    status.Bind("incidents", &incidents_);
    status.Bind("components", &components_);
    status.Bind("hasPage", &hasPage_);
    status.Bind("headset", &headset_);
    status.BindEventCallback("incident",
                             [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                             {
                                 const int i = a.empty() ? -1 : a[0].Get<int>(-1);
                                 if(i >= 0 && static_cast<size_t>(i) < incidentUrls_.size() &&
                                    !incidentUrls_[static_cast<size_t>(i)].empty())
                                 {
                                     link_ = incidentUrls_[static_cast<size_t>(i)];
                                 }
                             });
    status.BindEventCallback("statusPage",
                             [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                             {
                                 if(!pageUrl_.empty())
                                 {
                                     link_ = pageUrl_;
                                 }
                             });
    statusModel_ = status.GetModelHandle();

    if(!news_->load("news.rml") || !status_->load("grid_status.rml"))
    {
        log::error("the lobby's boards couldn't be read");
        return false;
    }
    return true;
}

std::string LobbyBoards::image(const std::string& url)
{
    if(url.empty())
    {
        return {};
    }
    const auto known = images_.find(url);
    if(known != images_.end())
    {
        return known->second.empty() || known->second == kFailed ? std::string()
                                                                 : "image://" + known->second;
    }
    images_[url] = "";
    net::HttpRequest request;
    request.url = url;
    auto pictures = pictures_;
    JobSystem* jobs = &jobs_;
    group_->send(std::move(request),
                 [pictures, jobs, url](net::HttpResponse&& response)
                 {
                     if(!response.ok())
                     {
                         pictures->push({url, std::nullopt});
                         return;
                     }
                     jobs->submit([pictures, url, body = std::move(response.body)]
                                  { pictures->push({url, gui::decodeImage(body)}); });
                 });
    return {};
}

void LobbyBoards::picturesArrived()
{
    std::vector<Picture> pictures;
    if(!pictures_->drain(pictures))
    {
        return;
    }
    for(Picture& p : pictures)
    {
        if(!p.pixels)
        {
            log::warn("news: picture {} not had", p.url);
            images_[p.url] = kFailed;
            continue;
        }
        const std::string name = std::format("news-{}", nextImage_++);
        gui_.setImage(name, std::move(*p.pixels));
        images_[p.url] = name;
    }
    filled_.reset(); // shown with them
}

void LobbyBoards::fill()
{
    const auto now = std::chrono::system_clock::now();

    std::vector<Entry> entries;
    entryIds_.clear();
    for(const NewsEntry& n : source_.news())
    {
        entries.push_back({n.title, n.summary, n.sourceName, describeTime(n.published, now),
                           image(n.imageUrl), n.pinned});
        entryIds_.push_back(n.id);
    }
    entries_ = std::move(entries);
    fillOpened();
    newsState_ = !source_.news().empty() ? "shown" : source_.newsKnown() ? "empty" : "loading";
    newsModel_.DirtyAllVariables();

    if(const auto& g = source_.gridStatus())
    {
        statusState_ = "shown";
        indicator_ = g->indicator;
        description_ = g->description;
        stale_ = g->stale;
        checked_ = describeTime(g->fetched, now);
        announcements_.clear();
        for(const auto& a : g->announcements)
        {
            announcements_.push_back(
                {a.severity, a.title, {}, a.body, describeTime(a.posted, now)});
        }
        pageUrl_ = g->pageUrl;
        hasPage_ = !pageUrl_.empty();
        incidents_.clear();
        incidentUrls_.clear();
        for(const auto& i : g->incidents)
        {
            incidents_.push_back({"incident", i.name, capitalized(i.status), i.update,
                                  describeTime(i.updated, now), !i.url.empty()});
            incidentUrls_.push_back(i.url);
        }
        for(const auto& m : g->maintenance)
        {
            incidents_.push_back({"maintenance", m.name, capitalized(m.status), m.update,
                                  describeTime(m.starts, now), !m.url.empty()});
            incidentUrls_.push_back(m.url);
        }
        components_.clear();
        for(const auto& c : g->components)
        {
            if(c.status != "operational")
            {
                components_.push_back({c.name, c.group, Rml::String(componentStatus(c.status)),
                                       Rml::String(componentKind(c.status))});
            }
        }
    }
    statusModel_.DirtyAllVariables();
}

void LobbyBoards::fillOpened()
{
    const auto& news = source_.news();
    const auto n = std::ranges::find(news, openId_, &NewsEntry::id);
    if(openId_.empty() || n == news.end())
    {
        openId_.clear();
        opened_ = false;
        openUrl_.clear();
        newsModel_.DirtyAllVariables();
        return;
    }
    opened_ = true;
    openTitle_ = n->title;
    openSource_ = n->sourceName;
    openWhen_ = describeTime(n->published, std::chrono::system_clock::now());
    openImage_ = image(n->imageUrl);
    openText_ = !n->body.empty() ? n->body : n->summary;
    openUrl_ = n->url;
    openLink_ = !openUrl_.empty();
    newsModel_.DirtyAllVariables();
}

float LobbyBoards::scrollTop(float to)
{
    Rml::Element* scroll =
        news_->document() ? news_->document()->GetElementById("scroll") : nullptr;
    if(!scroll)
    {
        return 0.0f;
    }
    const float was = scroll->GetScrollTop();
    scroll->SetScrollTop(to);
    return was;
}

void LobbyBoards::setHeadset(bool headset)
{
    if(headset != headset_)
    {
        headset_ = headset;
        newsModel_.DirtyVariable("headset");
        statusModel_.DirtyVariable("headset");
    }
}

void LobbyBoards::update()
{
    picturesArrived();
    const auto now = std::chrono::steady_clock::now();
    if(filled_ != source_.version() || now - filledAt_ > kRetell)
    {
        fill();
        filled_ = source_.version();
        filledAt_ = now;
    }
    news_->update();
    if(scrollTo_)
    {
        scrollTop(*scrollTo_);
        scrollTo_.reset();
        news_->update();
    }
    status_->update();
}

void LobbyBoards::render(SDL_GPUCommandBuffer* commands)
{
    news_->render(commands);
    status_->render(commands);
}

void LobbyBoards::reload()
{
    news_->reload();
    status_->reload();
}

} // namespace viewer
