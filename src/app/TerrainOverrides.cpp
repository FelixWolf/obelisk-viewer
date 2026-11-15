#include "TerrainOverrides.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

namespace viewer
{

TerrainOverrides::TerrainOverrides(net::HttpClient& http)
    : http_(http), requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void TerrainOverrides::request(uint64_t region, std::string_view capability)
{
    if(capability.empty())
    {
        return;
    }
    std::string& asked = asked_[region];
    if(asked == capability)
    {
        return;
    }
    asked = std::string(capability);
    net::HttpRequest request{.url = asked};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    requests_->send(std::move(request), [replies = replies_, region](net::HttpResponse&& response)
                    { replies->push({region, std::move(response)}); });
}

std::optional<TerrainOverrides::Layers> TerrainOverrides::parse(std::string_view body)
{
    const auto parsed = llsd::parse(body);
    if(!parsed || !(*parsed)["success"].asBoolean())
    {
        return std::nullopt;
    }
    const llsd::Value& overrides = (*parsed)["overrides"];
    if(!overrides.isArray() || overrides.size() < 4)
    {
        return std::nullopt;
    }
    Layers layers;
    for(size_t i = 0; i < 4; i++)
    {
        // An empty map changes nothing.
        if(overrides[i].isMap() && overrides[i].size() > 0)
        {
            layers[i] = assets::MaterialOverride::fromLlsd(overrides[i]);
        }
    }
    return layers;
}

void TerrainOverrides::update()
{
    replies_->drain(drained_);
    for(const Reply& r : drained_)
    {
        if(!asked_.contains(r.region))
        {
            continue; // cleared since
        }
        if(!r.response.ok())
        {
            log::warn("terrain overrides: the region's request failed (HTTP {})",
                      r.response.status);
            continue;
        }
        if(const auto layers = parse(r.response.bodyText()))
        {
            overrides_[r.region] = *layers;
            revision_++;
        }
        else
        {
            log::warn("terrain overrides: the region's reply isn't a successful one");
        }
    }
    drained_.clear();
}

const TerrainOverrides::Layers* TerrainOverrides::overrides(uint64_t region) const
{
    const auto it = overrides_.find(region);
    return it != overrides_.end() ? &it->second : nullptr;
}

void TerrainOverrides::clear()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    asked_.clear();
    overrides_.clear();
    revision_++;
}

} // namespace viewer
