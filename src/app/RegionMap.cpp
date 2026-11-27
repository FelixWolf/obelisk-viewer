#include "RegionMap.hpp"

#include <cmath>
#include <cstdlib>
#include <format>

namespace viewer
{

namespace
{

constexpr double kRegionWidth = 256.0;
// Pictures kept this far beyond those shown (in regions), for going back.
constexpr int kKeepMargin = 1;

} // namespace

std::string mapTileUrl(std::string_view server, uint32_t x, uint32_t y)
{
    std::string url(server);
    if(!url.empty() && url.back() != '/')
    {
        url += '/';
    }
    return url + std::format("map-1-{}-{}-objects.jpg", x, y);
}

std::optional<glm::vec2> mapPlace(glm::uvec2 centre, glm::ivec2 span, glm::dvec2 global)
{
    if(span.x <= 0 || span.y <= 0)
    {
        return std::nullopt;
    }
    const double west = (static_cast<double>(centre.x) - static_cast<double>(span.x / 2)) * kRegionWidth;
    const double south = (static_cast<double>(centre.y) - static_cast<double>(span.y / 2)) * kRegionWidth;
    const double across = (global.x - west) / (static_cast<double>(span.x) * kRegionWidth);
    const double up = (global.y - south) / (static_cast<double>(span.y) * kRegionWidth);
    if(across < 0.0 || across > 1.0 || up < 0.0 || up > 1.0)
    {
        return std::nullopt;
    }
    return glm::vec2(static_cast<float>(across), static_cast<float>(1.0 - up));
}

RegionMap::RegionMap(net::HttpClient& http, JobSystem& jobs, gui::Gui& gui)
    : http_(http), jobs_(jobs), gui_(gui), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

RegionMap::~RegionMap()
{
    group_.reset(); // no answers after this
    forgetAll();
}

void RegionMap::setServer(std::string url)
{
    if(url == server_)
    {
        return;
    }
    server_ = std::move(url);
    // Asked again of the new one.
    generation_++;
    for(auto& [k, region] : regions_)
    {
        if(region.picture == Picture::Coming || region.picture == Picture::Failed)
        {
            region.picture = Picture::None;
        }
    }
    version_++;
}

void RegionMap::clear()
{
    forgetAll();
    regions_.clear();
    server_.clear();
    centre_.reset();
    request_.reset();
    generation_++;
    version_++;
}

void RegionMap::forgetAll()
{
    for(auto& [k, region] : regions_)
    {
        if(region.picture == Picture::Here)
        {
            gui_.removeImage(region.imageName);
        }
        region.picture = Picture::None;
        region.imageName.clear();
    }
}

void RegionMap::forgetFar()
{
    if(!centre_)
    {
        return;
    }
    for(auto& [k, region] : regions_)
    {
        const auto x = static_cast<int64_t>(k >> 32);
        const auto y = static_cast<int64_t>(k & 0xFFFFFFFFu);
        const bool far =
            std::abs(x - static_cast<int64_t>(centre_->x)) > kRadiusX + kKeepMargin ||
            std::abs(y - static_cast<int64_t>(centre_->y)) > kRadiusY + kKeepMargin;
        if(far && region.picture == Picture::Here)
        {
            gui_.removeImage(region.imageName);
            region.imageName.clear();
            region.picture = Picture::None;
        }
    }
}

void RegionMap::fetch(uint32_t x, uint32_t y, Region& region)
{
    if(region.picture != Picture::None || server_.empty())
    {
        return;
    }
    region.picture = Picture::Coming;
    net::HttpRequest request;
    request.url = mapTileUrl(server_, x, y);
    auto arrived = arrived_;
    JobSystem* jobs = &jobs_;
    const uint64_t generation = generation_;
    const uint64_t k = key(x, y);
    // The workers outlive us (the app's), and the queue is shared.
    group_->send(std::move(request),
                 [arrived, jobs, generation, k](net::HttpResponse&& response)
                 {
                     if(!response.ok())
                     {
                         arrived->push({generation, k, std::nullopt});
                         return;
                     }
                     jobs->submit([arrived, generation, k, body = std::move(response.body)]
                                  { arrived->push({generation, k, gui::decodeImage(body)}); });
                 });
}

void RegionMap::update(std::optional<glm::uvec2> centre)
{
    if(centre != centre_)
    {
        centre_ = centre;
        version_++;
        if(centre_)
        {
            const auto low = [](uint32_t v, uint32_t r) { return v >= r ? v - r : 0u; };
            request_ = Range{low(centre_->x, kRadiusX), low(centre_->y, kRadiusY),
                             centre_->x + kRadiusX, centre_->y + kRadiusY};
            forgetFar();
        }
    }
    if(centre_)
    {
        for(int dy = -kRadiusY; dy <= kRadiusY; dy++)
        {
            for(int dx = -kRadiusX; dx <= kRadiusX; dx++)
            {
                const int64_t x = static_cast<int64_t>(centre_->x) + dx;
                const int64_t y = static_cast<int64_t>(centre_->y) + dy;
                if(x < 0 || y < 0)
                {
                    continue;
                }
                const auto ux = static_cast<uint32_t>(x);
                const auto uy = static_cast<uint32_t>(y);
                fetch(ux, uy, regions_[key(ux, uy)]);
            }
        }
    }

    std::vector<Arrived> arrived;
    arrived_->drain(arrived);
    for(Arrived& a : arrived)
    {
        const auto found = regions_.find(a.key);
        if(a.generation != generation_ || found == regions_.end() ||
           found->second.picture != Picture::Coming)
        {
            continue; // not asked for now
        }
        Region& region = found->second;
        if(!a.pixels)
        {
            region.picture = Picture::Failed;
        }
        else
        {
            region.imageName =
                std::format("regionmap-{}-{}", a.key >> 32, a.key & 0xFFFFFFFFu);
            gui_.setImage(region.imageName, std::move(*a.pixels));
            region.picture = Picture::Here;
        }
        version_++;
    }
}

void RegionMap::mapBlocks(std::span<const Block> blocks)
{
    for(const Block& block : blocks)
    {
        // A name search's answer when there's no such region.
        if(block.x == 0 && block.y == 0)
        {
            continue;
        }
        Region& region = regions_[key(block.x, block.y)];
        if(region.name != block.name || region.image != block.image)
        {
            region.name = block.name;
            region.image = block.image;
            version_++;
        }
    }
}

std::vector<RegionMap::Tile> RegionMap::tiles() const
{
    std::vector<Tile> tiles;
    if(!centre_)
    {
        return tiles;
    }
    for(int dy = kRadiusY; dy >= -kRadiusY; dy--)
    {
        for(int dx = -kRadiusX; dx <= kRadiusX; dx++)
        {
            const int64_t x = static_cast<int64_t>(centre_->x) + dx;
            const int64_t y = static_cast<int64_t>(centre_->y) + dy;
            Tile tile;
            if(x < 0 || y < 0)
            {
                tiles.push_back(tile); // off the grid: water
                continue;
            }
            tile.x = static_cast<uint32_t>(x);
            tile.y = static_cast<uint32_t>(y);
            const auto found = regions_.find(key(tile.x, tile.y));
            if(found != regions_.end())
            {
                const Region& region = found->second;
                tile.name = region.name;
                if(region.picture == Picture::Here)
                {
                    tile.image = "image://" + region.imageName;
                }
                else if((region.picture == Picture::Failed || server_.empty()) &&
                        !region.image.isNull())
                {
                    tile.image = std::format("asset://{}?size=256", region.image.toString());
                }
            }
            tiles.push_back(std::move(tile));
        }
    }
    return tiles;
}

} // namespace viewer
