#pragma once

// World state for one session, built from the messages the regions send:
// terrain, objects, avatar appearances and animations.
//
// Handling a message is two steps: decodeMessage() turns it into a
// WorldUpdate without looking at any world state, and World::apply() makes
// the change. WorldThread does both on its own thread and publishes copies
// for rendering.
//
// Copies are cheap: objects, terrain, appearances and animations are held by
// shared pointers and copied on write, so a copy shares everything that
// hasn't changed since. A World is used by one thread at a time; copies
// may go to other threads (they share only immutable data).

#include "viewer/core/Uuid.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/Objects.hpp"
#include "viewer/world/TerrainDecoder.hpp"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <variant>
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

using ObjectMap = std::unordered_map<ObjectKey, std::shared_ptr<Object>, ObjectKeyHash>;

// An avatar's appearance (from AvatarAppearance): bakes, visual parameter
// values (one byte each, in the avatar definition's transmitted order) and
// hover offset.
struct AvatarAppearance
{
    TextureEntry faces;
    std::vector<uint8_t> visualParams;
    glm::vec3 hover{0.0f};
    uint64_t serial = 0; // bumped on every change (set by World)
};

// An animation an avatar plays, with the sequence number it was started with.
struct PlayingAnimation
{
    Uuid id;
    int32_t sequence = 0;
};

// What one message changes, decoded.
struct WorldUpdate
{
    struct Objects
    {
        std::vector<ObjectUpdateData> updates;
        uint32_t full = 0;
        uint32_t compressed = 0;
        uint32_t terse = 0;
        uint32_t undecodable = 0;
        // When the message arrived, and how fast the simulator was running
        // (RegionData.TimeDilation, 1 = real time).
        std::chrono::steady_clock::time_point received{};
        float timeDilation = 1.0f;
    };
    struct Kill
    {
        std::vector<uint32_t> localIds;
    };
    struct Terrain
    {
        std::vector<TerrainPatch> patches;
    };
    struct Handshake
    {
        TerrainComposition composition;
    };
    struct Appearance
    {
        Uuid avatar;
        AvatarAppearance appearance;
    };
    struct Animations
    {
        Uuid avatar;
        std::vector<PlayingAnimation> playing;
    };
    struct RegionRemoved
    {
    };

    uint64_t region = 0;
    // monostate: nothing for the world (or a message that didn't decode).
    std::variant<std::monostate, Objects, Kill, Terrain, Handshake, Appearance, Animations,
                 RegionRemoved>
        change;
};

// Decodes a message from the region with this handle (see
// Session::RegionMessage). Uses no world state; safe on any thread.
WorldUpdate decodeMessage(uint64_t regionHandle, const protocol::CircuitEvent& event);

class World
{
public:
    void apply(WorldUpdate update);
    // decodeMessage() and apply() in one go.
    void handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event);
    void removeRegion(uint64_t regionHandle);

    // Every region's terrain, as (handle, RegionTerrain) pairs.
    auto terrain() const
    {
        return std::views::transform(
            terrain_, [](const auto& e)
            { return std::pair<uint64_t, const RegionTerrain&>(e.first, *e.second); });
    }
    size_t regionCount() const { return terrain_.size(); }
    const RegionTerrain* terrain(uint64_t regionHandle) const;

    // Every object, as (ObjectKey, Object) pairs.
    auto objects() const
    {
        return std::views::transform(
            objects_, [](const auto& e)
            { return std::pair<const ObjectKey&, const Object&>(e.first, *e.second); });
    }
    size_t objectCount() const { return objects_.size(); }
    const Object* object(uint64_t region, uint32_t localId) const;
    using Appearance = AvatarAppearance;
    using PlayingAnimation = world::PlayingAnimation;
    const Appearance* appearance(const Uuid& avatar) const;
    // The animations an avatar or animated object is playing (from
    // AvatarAnimation or ObjectAnimation: the full list each time).
    const std::vector<PlayingAnimation>* animations(const Uuid& id) const;
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
    // Object id -> where it is, so an object that moves to another region
    // (an avatar walking across) replaces its copy in the old one, whose
    // simulator doesn't always kill it. Only the world being updated needs
    // it: copies (snapshots) start without and rebuild it if ever updated.
    struct IdIndex
    {
        std::unordered_map<Uuid, ObjectKey> keys;
        bool valid = true;

        IdIndex() = default;
        IdIndex(const IdIndex&) : valid(false) {}
        IdIndex(IdIndex&&) = default;
        IdIndex& operator=(const IdIndex&)
        {
            keys.clear();
            valid = false;
            return *this;
        }
        IdIndex& operator=(IdIndex&&) = default;
        ~IdIndex() = default;
    };

    void applyObject(uint64_t regionHandle, const ObjectUpdateData& update,
                     std::chrono::steady_clock::time_point received, float timeDilation);
    IdIndex& ids();
    // Removes the object and everything linked or attached to it.
    void removeWithChildren(const ObjectKey& key);
    void forgetId(const ObjectKey& key, const Object& object);

    // A region's terrain to change: its own copy if a copy of the world
    // still shares it.
    RegionTerrain& region(uint64_t handle);

    std::map<uint64_t, std::shared_ptr<RegionTerrain>> terrain_;
    ObjectMap objects_;
    IdIndex ids_;
    std::unordered_map<Uuid, std::shared_ptr<const Appearance>> appearances_;
    uint64_t appearanceSerial_ = 0;
    std::unordered_map<Uuid, std::shared_ptr<const std::vector<PlayingAnimation>>> animations_;
    uint64_t objectsRevision_ = 0;
    Stats stats_;
};

} // namespace viewer::world
