#pragma once

// World state for one session, built from the messages the regions send.
// So far: terrain heightmaps and terrain composition per region.
//
// Threading: single-threaded for now (the main thread feeds it). It will
// move to its own thread once object updates make it busy.

#include "viewer/core/Uuid.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/TerrainDecoder.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <vector>

namespace viewer::world
{

inline constexpr uint32_t kRegionWidth = 256; // meters (and heightmap samples)

// Which detail texture applies where (from RegionHandshake). Corners are
// southwest, northwest, southeast, northeast (the message's 00, 01, 10, 11).
struct TerrainComposition
{
    std::array<Uuid, 4> detailTextures{};
    std::array<float, 4> startHeight{};
    std::array<float, 4> heightRange{};
    float waterHeight = 20.0f;
};

struct RegionTerrain
{
    uint64_t handle = 0;
    std::vector<float> heights = std::vector<float>(kRegionWidth * kRegionWidth, 0.0f);
    std::bitset<(kRegionWidth / kPatchSize) * (kRegionWidth / kPatchSize)> patchesReceived;
    TerrainComposition composition{};
    bool hasComposition = false;
    uint32_t revision = 0; // bumped on every change

    float height(uint32_t x, uint32_t y) const { return heights[y * kRegionWidth + x]; }
    bool complete() const { return patchesReceived.all(); }
};

class World
{
public:
    // A message from the region with this handle (see Session::RegionMessage).
    void handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void removeRegion(uint64_t regionHandle);

    const std::map<uint64_t, RegionTerrain>& terrain() const { return terrain_; }
    const RegionTerrain* terrain(uint64_t regionHandle) const;

private:
    RegionTerrain& region(uint64_t handle);

    std::map<uint64_t, RegionTerrain> terrain_;
};

} // namespace viewer::world
