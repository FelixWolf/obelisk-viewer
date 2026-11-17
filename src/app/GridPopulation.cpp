#include "GridPopulation.hpp"

#include <algorithm>
#include <cctype>

namespace viewer
{

namespace
{

constexpr uint32_t kRegionWidth = 256;

uint64_t handleAt(uint32_t x, uint32_t y)
{
    return static_cast<uint64_t>(x / kRegionWidth * kRegionWidth) << 32 |
           static_cast<uint64_t>(y / kRegionWidth * kRegionWidth);
}

} // namespace

std::string GridPopulation::key(std::string_view name)
{
    std::string k(name);
    std::ranges::transform(k, k.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return k;
}

std::optional<int> GridPopulation::people(std::string_view region, Clock::time_point now)
{
    if(region.empty())
    {
        return std::nullopt;
    }
    const Lookup& lookup = regions_[key(region)];
    if(!lookup.handle)
    {
        return std::nullopt;
    }
    Count& c = counts_[*lookup.handle];
    // Asked, and no spots came: nobody's there.
    if(c.asked && (!c.counted || *c.counted < *c.asked) && now - *c.asked >= kAnswerWait)
    {
        c.people = 0;
        c.counted = c.asked;
        version_++;
    }
    c.wanted = !c.counted || now - *c.counted >= kKeepCount;
    return c.people;
}

GridPopulation::Requests GridPopulation::requests(Clock::time_point now)
{
    Requests r;
    for(auto& [name, lookup] : regions_)
    {
        if(r.regions.size() >= kAtOnce)
        {
            break;
        }
        if(!lookup.handle && !lookup.missing && (!lookup.asked || now - *lookup.asked > kRetry))
        {
            lookup.asked = now;
            r.regions.push_back(name);
        }
    }
    for(auto& [handle, c] : counts_)
    {
        if(r.agents.size() >= kAtOnce)
        {
            break;
        }
        const bool waiting = c.asked && (!c.counted || *c.counted < *c.asked);
        if(c.wanted && !waiting)
        {
            c.wanted = false;
            c.asked = now;
            r.agents.push_back(handle);
        }
    }
    return r;
}

void GridPopulation::mapBlocks(std::span<const MapBlock> blocks)
{
    // Those found first: a name's echo at 0, 0 may come before or after it.
    for(const MapBlock& block : blocks)
    {
        if(block.name.empty() || (block.x == 0 && block.y == 0))
        {
            continue;
        }
        Lookup& lookup = regions_[key(block.name)];
        const uint64_t handle = static_cast<uint64_t>(block.x) * kRegionWidth << 32 |
                                static_cast<uint64_t>(block.y) * kRegionWidth;
        lookup.missing = false;
        if(lookup.handle != handle)
        {
            lookup.handle = handle;
            version_++;
        }
    }
    for(const MapBlock& block : blocks)
    {
        if(block.name.empty() || block.x != 0 || block.y != 0)
        {
            continue;
        }
        const auto lookup = regions_.find(key(block.name));
        if(lookup != regions_.end() && !lookup->second.handle)
        {
            lookup->second.missing = true;
        }
    }
}

void GridPopulation::agentLocations(std::span<const Spot> spots, Clock::time_point now)
{
    // An answer's spots replace what was known of their regions.
    std::map<uint64_t, int> people;
    for(const Spot& spot : spots)
    {
        people[handleAt(spot.x, spot.y)] += std::max(spot.count, 0);
    }
    for(const auto& [handle, n] : people)
    {
        Count& c = counts_[handle];
        c.people = n;
        c.counted = now;
        c.wanted = false;
        version_++;
    }
}

void GridPopulation::clear()
{
    regions_.clear();
    counts_.clear();
    version_++;
}

} // namespace viewer
