#pragma once

// The map on the main menu's Home tab: the region we're in and its
// neighbours (kColumns by kRows regions, ours in the middle: wider than
// tall, to use more of the card), as the world map draws them.
//
// Each region's picture is the grid's map tile (the login's map-server-url:
// map-1-X-Y-objects.jpg, one region a tile, as the official viewer's world
// map at its closest), from the web and decoded on the workers, given to the
// interface as image://regionmap-X-Y. With no map server, or a tile it
// hasn't got, the region's map image from MapBlockReply (its MapImageID, an
// asset) instead. A region with neither shows as water.
//
// The neighbours' names come from the map too: a MapBlockRequest for the
// regions shown (takeRequest(): the app sends it, and gives the answers
// to mapBlocks()). Per session (clear() between).
//
// Regions are taken to be 256 m across (as GridPopulation's).

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/gui/Gui.hpp"
#include "viewer/net/HttpClient.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viewer
{

// The map server's tile of a region (grid coordinates, in regions); the
// server's address as the login gives it (with or without its last '/').
std::string mapTileUrl(std::string_view server, uint32_t x, uint32_t y);

// Where a global position (meters) is on a map of span.x by span.y regions
// (odd numbers) with `centre` (grid coordinates, in regions) in the middle:
// across from its left and down from its top, 0 to 1 each (north is up).
// Nothing if it's off the map.
std::optional<glm::vec2> mapPlace(glm::uvec2 centre, glm::ivec2 span, glm::dvec2 global);

class RegionMap
{
public:
    // Regions on each side of ours: east and west, north and south.
    static constexpr int kRadiusX = 2;
    static constexpr int kRadiusY = 1;
    static constexpr int kColumns = 2 * kRadiusX + 1;
    static constexpr int kRows = 2 * kRadiusY + 1;
    static constexpr glm::ivec2 kSpan{kColumns, kRows};

    RegionMap(net::HttpClient& http, JobSystem& jobs, gui::Gui& gui);
    ~RegionMap();
    RegionMap(const RegionMap&) = delete;
    RegionMap& operator=(const RegionMap&) = delete;

    // The grid's map server (the login's map-server-url; empty: none).
    void setServer(std::string url);
    // Logged out: nothing kept (the next login may be another grid).
    void clear();

    // The region in the middle (grid coordinates, in regions; nothing: not
    // in one), and the pictures that have come. Once a frame while shown.
    void update(std::optional<glm::uvec2> centre);
    std::optional<glm::uvec2> centre() const { return centre_; }

    // The regions to ask the map about (MapBlockRequest), once a centre.
    struct Range
    {
        uint32_t minX = 0;
        uint32_t minY = 0;
        uint32_t maxX = 0;
        uint32_t maxY = 0;
    };
    std::optional<Range> takeRequest() { return std::exchange(request_, std::nullopt); }

    // MapBlockReply: regions, where they are and their map images.
    struct Block
    {
        std::string name;
        uint32_t x = 0; // in regions
        uint32_t y = 0;
        Uuid image;
    };
    void mapBlocks(std::span<const Block> blocks);

    // The regions shown, the northern row first, west to east in each.
    struct Tile
    {
        uint32_t x = 0;
        uint32_t y = 0;
        std::string name;  // empty: not known (or no region there)
        std::string image; // image://... or asset://...; empty: none (water)
    };
    std::vector<Tile> tiles() const;

    // Changes when tiles() might have.
    uint64_t version() const { return version_; }

private:
    enum class Picture
    {
        None,   // not asked for
        Coming, // from the map server
        Here,   // image://<image>
        Failed, // the server hasn't one
    };
    struct Region
    {
        std::string name;
        Uuid image; // MapImageID
        Picture picture = Picture::None;
        std::string imageName; // in the interface, while Here
    };
    struct Arrived
    {
        uint64_t generation = 0;
        uint64_t key = 0;
        std::optional<gui::Pixels> pixels;
    };
    static uint64_t key(uint32_t x, uint32_t y) { return static_cast<uint64_t>(x) << 32 | y; }
    void fetch(uint32_t x, uint32_t y, Region& region);
    // Lets go of the pictures of regions far from the centre.
    void forgetFar();
    void forgetAll();

    net::HttpClient& http_;
    JobSystem& jobs_;
    gui::Gui& gui_;
    std::unique_ptr<net::HttpRequestGroup> group_;
    // Shared with the workers' jobs, which may finish after we've gone.
    std::shared_ptr<MessageQueue<Arrived>> arrived_ = std::make_shared<MessageQueue<Arrived>>();

    std::string server_;
    std::optional<glm::uvec2> centre_;
    std::optional<Range> request_;
    std::map<uint64_t, Region> regions_; // by key()
    uint64_t generation_ = 0;            // of the pictures asked for (a session's)
    uint64_t version_ = 0;
};

} // namespace viewer
