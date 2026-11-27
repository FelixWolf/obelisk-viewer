#include "AvatarRenderInfo.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <climits>
#include <format>

namespace viewer
{

llsd::Value avatarRenderReportBody(const std::vector<AvatarRenderReport>& reports)
{
    llsd::Map agents;
    for(const AvatarRenderReport& r : reports)
    {
        if(r.complexity == 0)
        {
            continue; // as the official viewer: nothing known yet
        }
        llsd::Map info;
        // Signed in LLSD: held to the largest.
        info.set("weight", llsd::Value(static_cast<int32_t>(std::min<uint32_t>(
                               r.complexity, static_cast<uint32_t>(INT32_MAX)))));
        info.set("tooComplex", llsd::Value(r.tooComplex));
        agents.set(r.avatar.toString(), llsd::Value(std::move(info)));
    }
    llsd::Map body;
    body.set("agents", llsd::Value(std::move(agents)));
    return llsd::Value(std::move(body));
}

std::optional<AvatarRenderAnswer> parseAvatarRenderAnswer(const llsd::Value& body)
{
    if(!body.isMap())
    {
        return std::nullopt;
    }
    AvatarRenderAnswer answer;
    if(const llsd::Map* agents = body["agents"].get<llsd::Map>())
    {
        for(const auto& [name, info] : *agents)
        {
            const auto id = Uuid::parse(name);
            if(id && info.isMap() && info.has("weight"))
            {
                answer.weights[*id] =
                    static_cast<uint32_t>(std::max(0, info["weight"].asInteger()));
            }
        }
    }
    if(body.has("reportinglimit") && body.has("overlimit"))
    {
        answer.reporting = body["reportinglimit"].asInteger();
        answer.overLimit = body["overlimit"].asInteger();
    }
    return answer;
}

AvatarRenderInfo::AvatarRenderInfo(net::HttpClient& http)
    : http_(http), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void AvatarRenderInfo::clear()
{
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    std::vector<Answer> stale;
    answers_.drain(stale);
    regions_.clear();
    seen_.reset();
    main_ = 0;
}

std::optional<uint32_t> AvatarRenderInfo::reported(const Uuid& avatar) const
{
    for(const auto& [handle, region] : regions_)
    {
        if(const auto it = region.weights.find(avatar); it != region.weights.end())
        {
            return it->second;
        }
    }
    return std::nullopt;
}

void AvatarRenderInfo::update(const std::vector<uint64_t>& regions, uint64_t main,
                              const Capability& capability, const Reports& reports,
                              std::chrono::steady_clock::time_point now)
{
    if(main != main_)
    {
        main_ = main;
        seen_.reset(); // another region's people
    }

    // Answers.
    answers_.drain(drained_);
    for(Answer& a : drained_)
    {
        const auto it = regions_.find(a.region);
        if(a.post || it == regions_.end())
        {
            continue; // (a POST's answer says nothing we use)
        }
        Region& region = it->second;
        region.getting = false;
        if(!a.response.ok())
        {
            continue; // asked again in a while anyway
        }
        const auto body = llsd::parse(a.response.bodyText());
        const auto answer = body ? parseAvatarRenderAnswer(*body) : std::nullopt;
        if(!answer)
        {
            continue;
        }
        region.weights = answer->weights;
        if(a.region == main_ && answer->reporting && answer->overLimit)
        {
            seen_ = Seen{*answer->reporting, *answer->overLimit};
        }
    }

    // Regions gone are forgotten; new ones are asked at once, and told a
    // little later (once we've had a chance to add their avatars up).
    std::erase_if(regions_,
                  [&](const auto& entry) { return !std::ranges::contains(regions, entry.first); });
    for(const uint64_t handle : regions)
    {
        auto [it, added] = regions_.try_emplace(handle);
        Region& region = it->second;
        if(added)
        {
            region.nextGet = now;
            region.nextPost = now + kFirstPost;
        }
        const std::string url = capability(handle);
        if(url.empty())
        {
            continue;
        }
        if(now >= region.nextGet && !region.getting)
        {
            region.nextGet = now + kGetInterval;
            region.getting = true;
            net::HttpRequest r{.method = net::HttpMethod::Get, .url = url};
            r.headers.add("Accept", std::string(llsd::kXmlContentType));
            r.timeout = kGetInterval;
            group_->send(std::move(r), [this, handle](net::HttpResponse&& response)
                         { answers_.push({handle, false, std::move(response)}); });
        }
        if(now >= region.nextPost)
        {
            const std::vector<AvatarRenderReport> told = reports(handle);
            const auto count = std::ranges::count_if(told, [](const AvatarRenderReport& r)
                                                     { return r.complexity > 0; });
            // Longer apart with more avatars around.
            region.nextPost = now + kPostInterval + std::chrono::seconds(2 * count);
            if(count == 0)
            {
                continue;
            }
            const std::string xml = llsd::toXml(avatarRenderReportBody(told));
            net::HttpRequest r{.method = net::HttpMethod::Post, .url = url};
            r.headers.add("Content-Type", std::string(llsd::kXmlContentType));
            r.headers.add("Accept", std::string(llsd::kXmlContentType));
            r.body.assign(xml.begin(), xml.end());
            group_->send(std::move(r), [this, handle](net::HttpResponse&& response)
                         { answers_.push({handle, true, std::move(response)}); });
        }
    }
}

} // namespace viewer
