#include "LegacyMaterialManager.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <format>

namespace viewer
{

namespace
{

constexpr size_t kPerRequest = 50; // MATERIALS_GET_MAX_ENTRIES
constexpr size_t kMaxRequests = 1;
constexpr auto kRequestInterval = std::chrono::seconds(1); // RenderMaterialsCapability
constexpr uint8_t kMaxAttempts = 3;

} // namespace

LegacyMaterialManager::LegacyMaterialManager(net::HttpClient& http)
    : http_(http), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void LegacyMaterialManager::setCapabilityUrl(std::string url)
{
    if(url == url_)
    {
        return;
    }
    url_ = std::move(url);
    // Requests in flight belong to the old URL: drop them and ask again.
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    requests_ = 0;
    std::vector<Answer> stale;
    answers_.drain(stale);
    for(auto& [id, e] : entries_)
    {
        if(e.state == State::Requested)
        {
            e.state = State::Queued;
            queue_.push_back(id);
        }
    }
}

std::string_view LegacyMaterialManager::stateName(const Uuid& id) const
{
    const auto it = entries_.find(id);
    if(it == entries_.end())
    {
        return "unknown";
    }
    switch(it->second.state)
    {
    case State::Queued:
        return "queued";
    case State::Requested:
        return "requested";
    case State::Ready:
        return "ready";
    case State::Missing:
        return "missing";
    }
    return "?";
}

std::shared_ptr<const assets::LegacyMaterial> LegacyMaterialManager::get(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    auto [it, inserted] = entries_.try_emplace(id);
    if(inserted)
    {
        queue_.push_back(id);
    }
    return it->second.material;
}

void LegacyMaterialManager::put(const Uuid& id, assets::LegacyMaterial material)
{
    std::erase(queue_, id);
    Entry& e = entries_[id];
    e.state = State::Ready;
    e.material = std::make_shared<const assets::LegacyMaterial>(std::move(material));
    revision_++;
}

void LegacyMaterialManager::update()
{
    answers_.drain(drained_);
    for(Answer& a : drained_)
    {
        requests_--;
        std::vector<std::pair<Uuid, assets::LegacyMaterial>> materials;
        if(a.response.status == 503 || a.response.status == 429)
        {
            // Asked too fast: again later.
            for(const Uuid& id : a.asked)
            {
                auto it = entries_.find(id);
                if(it != entries_.end() && it->second.state == State::Requested &&
                   it->second.attempts < kMaxAttempts)
                {
                    it->second.state = State::Queued;
                    queue_.push_back(id);
                }
            }
            nextRequest_ = std::chrono::steady_clock::now() + 2 * kRequestInterval;
        }
        else if(a.response.ok())
        {
            auto body = llsd::parse(std::string_view(
                reinterpret_cast<const char*>(a.response.body.data()), a.response.body.size()));
            auto parsed = body ? assets::parseLegacyMaterials(*body)
                               : std::unexpected(std::string("unreadable answer"));
            if(parsed)
            {
                materials = std::move(*parsed);
            }
            else
            {
                log::warn("legacy materials: {}", parsed.error());
            }
        }
        else
        {
            log::warn("legacy materials request failed: {}",
                      a.response.error != net::HttpError::None
                          ? std::string(net::errorName(a.response.error))
                          : std::format("HTTP {}", a.response.status));
        }
        for(auto& [id, material] : materials)
        {
            auto it = entries_.find(id);
            if(it != entries_.end())
            {
                it->second.material =
                    std::make_shared<const assets::LegacyMaterial>(std::move(material));
                it->second.state = State::Ready;
                revision_++;
            }
        }
        // Whatever the answer left out isn't there.
        for(const Uuid& id : a.asked)
        {
            auto it = entries_.find(id);
            if(it != entries_.end() && it->second.state == State::Requested)
            {
                it->second.state = State::Missing;
                revision_++;
            }
        }
    }
    drained_.clear();

    const auto now = std::chrono::steady_clock::now();
    while(requests_ < kMaxRequests && !queue_.empty() && !url_.empty() && now >= nextRequest_)
    {
        std::vector<Uuid> asked;
        while(!queue_.empty() && asked.size() < kPerRequest)
        {
            const Uuid id = queue_.back();
            queue_.pop_back();
            auto it = entries_.find(id);
            if(it != entries_.end() && it->second.state == State::Queued)
            {
                it->second.state = State::Requested;
                it->second.attempts++;
                asked.push_back(id);
            }
        }
        if(asked.empty())
        {
            continue;
        }
        const std::string xml = llsd::toXml(assets::legacyMaterialRequest(asked));
        net::HttpRequest request{.method = net::HttpMethod::Post, .url = url_};
        request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
        request.headers.add("Accept", std::string(llsd::kXmlContentType));
        request.body.assign(xml.begin(), xml.end());
        request.timeout = std::chrono::seconds(60);
        requests_++;
        nextRequest_ = now + kRequestInterval;
        group_->send(std::move(request),
                     [this, asked = std::move(asked)](net::HttpResponse&& response) mutable
                     { answers_.push({std::move(asked), std::move(response)}); });
    }
}

LegacyMaterialManager::Stats LegacyMaterialManager::stats() const
{
    Stats s;
    for(const auto& [id, e] : entries_)
    {
        s.ready += e.state == State::Ready;
        s.missing += e.state == State::Missing;
        s.loading += e.state == State::Queued || e.state == State::Requested;
    }
    return s;
}

} // namespace viewer
