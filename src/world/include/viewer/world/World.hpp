#pragma once

// World state for one session, built from the messages the regions send.
// So far: terrain heightmaps and terrain composition per region.
//
// Threading: single-threaded for now (the main thread feeds it). It will
// move to its own thread once object updates make it busy.

#include "viewer/core/Uuid.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/Objects.hpp"
#include "viewer/world/TerrainDecoder.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <unordered_map>
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

// Objects are identified by region and local id (the full UUID also
// exists, but updates refer to the local id).
struct ObjectKey
{
    uint64_t region = 0;
    uint32_t localId = 0;
    bool operator==(const ObjectKey&) const = default;
};

struct ObjectKeyHash
{
    size_t operator()(const ObjectKey& k) const noexcept
    {
        return std::hash<uint64_t>{}(k.region * 0x9E3779B97F4A7C15ull ^ k.localId);
    }
};

using ObjectMap = std::unordered_map<ObjectKey, Object, ObjectKeyHash>;

class World
{
public:
    // A message from the region with this handle (see Session::RegionMessage).
    void handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void removeRegion(uint64_t regionHandle);

    const std::map<uint64_t, RegionTerrain>& terrain() const { return terrain_; }
    const RegionTerrain* terrain(uint64_t regionHandle) const;

    const ObjectMap& objects() const { return objects_; }
    const Object* object(uint64_t region, uint32_t localId) const;
    // An avatar's appearance texture entry (bakes), from AvatarAppearance.
    const TextureEntry* appearance(const Uuid& avatar) const;
    // Bumped whenever any object is added, changed or removed.
    uint64_t objectsRevision() const { return objectsRevision_; }

    struct Stats
    {
        uint64_t fullUpdates = 0;
        uint64_t compressedUpdates = 0;
        uint64_t terseUpdates = 0;
        uint64_t undecodable = 0; // object updates we couldn't parse
    };
    const Stats& stats() const { return stats_; }

private:
    void handleObjectUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void handleCompressedUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void handleTerseUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void apply(uint64_t regionHandle, const ObjectUpdateData& update);

    RegionTerrain& region(uint64_t handle);

    std::map<uint64_t, RegionTerrain> terrain_;
    ObjectMap objects_;
    std::unordered_map<Uuid, TextureEntry> appearances_;
    uint64_t objectsRevision_ = 0;
    Stats stats_;
};

} // namespace viewer::world
