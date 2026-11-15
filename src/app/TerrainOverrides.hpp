#pragma once

// PBR terrain's material overrides, per region: the four layers' changes
// to their materials (as faces' overrides), which a region's estate
// managers set. Asked once of each region's ModifyRegion capability, as
// the official viewer does after RegionHandshake (llpbrterrainfeatures.cpp
// queryRegionCoro: a GET answering {success, overrides: [four maps]}).

#include "viewer/assets/Material.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/net/HttpClient.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class TerrainOverrides
{
public:
    using Layers = std::array<std::optional<assets::MaterialOverride>, 4>;

    explicit TerrainOverrides(net::HttpClient& http);

    // A region and its ModifyRegion capability (empty: none yet): asks
    // once per region (again if its capability changes). Call every frame
    // for the regions in view; handles the replies.
    void request(uint64_t region, std::string_view capability);
    void update();
    // A region's overrides (none until they arrive, or if it has none).
    const Layers* overrides(uint64_t region) const;
    // Bumped whenever a region's arrive.
    uint64_t revision() const { return revision_; }
    void clear();

    // The reply's overrides; nothing if it isn't a successful one.
    static std::optional<Layers> parse(std::string_view body);

private:
    struct Reply
    {
        uint64_t region = 0;
        net::HttpResponse response;
    };
    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<Reply>> replies_ = std::make_shared<MessageQueue<Reply>>();
    std::vector<Reply> drained_;
    std::unordered_map<uint64_t, std::string> asked_; // capability asked, by region
    std::unordered_map<uint64_t, Layers> overrides_;
    uint64_t revision_ = 0;
};

} // namespace viewer
