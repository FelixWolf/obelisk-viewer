#pragma once

// How many people are in a region, from the world map (as the official
// viewer's map counts them): where the region is (MapNameRequest, answered
// by MapBlockReply; kept for the session), then where the people in it are
// (MapItemRequest for agent locations with its handle: a few spots, each
// with how many are there; MapItemReply). A count is kept five minutes, then
// asked for again, but only when someone wants it.
//
// A region's answer doesn't say which region it is for: its spots do (their
// global positions). An empty region has none, so a region asked for that
// has no spots in an answer within kAnswerWait is taken to be empty.
//
// No messages of its own: the app sends what requests() says and gives it
// the answers (mapBlocks, agentLocations). Per session (clear() between).
//
// Regions are taken to be 256 m across: the message template's MapBlockReply
// has no Size block, so a bigger (var) region's people beyond its first
// 256 m aren't counted.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace viewer
{

class GridPopulation
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kKeepCount = std::chrono::minutes(5);
    // A region with no spots in an answer by then is empty.
    static constexpr auto kAnswerWait = std::chrono::seconds(5);
    // A region's name asked again if no answer by then.
    static constexpr auto kRetry = std::chrono::seconds(30);
    // Asked for at once (then a frame's wait).
    static constexpr size_t kAtOnce = 8;

    // People in a region (by name, any case) now; nothing if not known yet
    // (it's asked for: requests()). An old count still shows while a new
    // one comes.
    std::optional<int> people(std::string_view region, Clock::time_point now);

    // What to send now: regions' names to look up, and regions (handles)
    // to ask where the people in them are.
    struct Requests
    {
        std::vector<std::string> regions;
        std::vector<uint64_t> agents;
    };
    Requests requests(Clock::time_point now);

    // MapBlockReply: the regions whose names start with what was asked
    // (the search ignores case; the names come as the regions have them),
    // and the name asked for at 0, 0 (with access 255) when there's no
    // region of that name. A name is found by a block of it, in any case,
    // not at 0, 0; missing (not asked again) if its only block is at 0, 0.
    struct MapBlock
    {
        std::string name;
        uint32_t x = 0; // in regions
        uint32_t y = 0;
    };
    void mapBlocks(std::span<const MapBlock> blocks);

    // MapItemReply (agent locations): spots, at global positions (meters),
    // with how many are at each; a region's count is its spots'.
    struct Spot
    {
        uint32_t x = 0;
        uint32_t y = 0;
        int count = 0;
    };
    void agentLocations(std::span<const Spot> spots, Clock::time_point now);

    // Changes when a number might have (a region found, people counted).
    uint64_t version() const { return version_; }
    void clear();

private:
    static std::string key(std::string_view name);

    struct Lookup
    {
        std::optional<uint64_t> handle; // found
        bool missing = false;           // there's no such region
        std::optional<Clock::time_point> asked;
    };
    struct Count
    {
        std::optional<int> people;
        std::optional<Clock::time_point> counted; // when
        std::optional<Clock::time_point> asked;   // the last request
        bool wanted = false;                      // to be asked
    };
    std::unordered_map<std::string, Lookup> regions_; // by key()
    std::map<uint64_t, Count> counts_;                // by region handle
    uint64_t version_ = 0;
};

} // namespace viewer
