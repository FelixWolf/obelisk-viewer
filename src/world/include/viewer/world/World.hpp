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

#include "viewer/assets/Material.hpp"
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
#include <unordered_set>
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
    // Between samples (bilinear), clamped to the region.
    float heightAt(float x, float y) const;
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
    struct Wind
    {
        WindField wind;
    };
    // An object's PBR material overrides, per face (GenericStreamingMessage
    // 0x4175): the whole set, replacing the last. `raw` is the message's
    // LLSD notation, for the object cache.
    // An object's sound set (AttachedSound), or only its gain changed
    // (AttachedSoundGainChange: `set` empty).
    struct Sound
    {
        Uuid object;
        std::optional<ObjectSound> set;
        float gain = 0.0f;
    };
    struct MaterialOverrides
    {
        uint32_t localId = 0;
        std::vector<std::pair<uint8_t, assets::MaterialOverride>> faces;
        std::string raw;
    };

    uint64_t region = 0;
    // monostate: nothing for the world (or a message that didn't decode).
    std::variant<std::monostate, Objects, Kill, Terrain, Handshake, Appearance, Animations,
                 RegionRemoved, MaterialOverrides, Wind, Sound>
        change;
};

// The overrides of one object's faces.
using FaceOverrides = std::vector<std::pair<uint8_t, assets::MaterialOverride>>;

// Reads an override message's LLSD notation (also from the object cache).
std::optional<WorldUpdate::MaterialOverrides> decodeMaterialOverrides(std::string_view notation);

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
    // A region's latest wind (LayerData), if it has sent any.
    const WindField* wind(uint64_t regionHandle) const;

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
    // An object's PBR material overrides (kept even before the object
    // arrives, as the official viewer does); null if none.
    const FaceOverrides* materialOverrides(const ObjectKey& key) const;
    // The animations an avatar or animated object is playing (from
    // AvatarAnimation or ObjectAnimation: the full list each time).
    const std::vector<PlayingAnimation>* animations(const Uuid& id) const;
    // Bumped whenever any object is added, changed or removed.
    uint64_t objectsRevision() const { return objectsRevision_; }
    // The same, but not bumped by motion alone (terse updates without a
    // texture entry): for what depends on what objects are, not where.
    uint64_t objectContentsRevision() const { return contentsRevision_; }
    // What changed since the snapshot before this one (WorldThread publishes
    // each to the render thread in turn): the objects updated, added or
    // removed, by key; `everything` when too much to list, or a region went.
    struct Changes
    {
        std::vector<ObjectKey> keys;
        bool everything = false;
    };
    const Changes& changes() const { return changes_; }
    // Which snapshot this is (each published one is the last's plus one).
    uint64_t serial() const { return serial_; }
    // A world made up on its own (not one of a WorldThread's snapshots):
    // whatever follows snapshots starts afresh from it, and from the next
    // snapshot after it.
    void standAlone()
    {
        changes_ = {{}, true};
        serial_ = ~0ull >> 1;
    }
    // Before publishing a copy: the next changes start afresh.
    void startNextSnapshot()
    {
        changes_ = {};
        serial_++;
    }

    // The avatars, and the animated objects' roots (prims with
    // animatedMesh), by key: few, among thousands of objects.
    const std::unordered_set<ObjectKey, ObjectKeyHash>& avatarKeys() const { return avatarKeys_; }
    const std::unordered_set<ObjectKey, ObjectKeyHash>& animatedKeys() const
    {
        return animatedKeys_;
    }

    // Bumped whenever any avatar's appearance changes.
    uint64_t appearanceRevision() const { return appearanceSerial_; }

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
    std::map<uint64_t, std::shared_ptr<const WindField>> wind_; // apart: it changes often
    ObjectMap objects_;
    IdIndex ids_;
    // AttachedSound for objects not here yet: applied if they come within
    // 15 s (llviewermessage.cpp PostponedSoundData).
    struct PendingSound
    {
        ObjectSound sound;
        std::chrono::steady_clock::time_point until;
    };
    std::unordered_map<Uuid, PendingSound> pendingSounds_;
    std::unordered_map<Uuid, std::shared_ptr<const Appearance>> appearances_;
    uint64_t appearanceSerial_ = 0;
    Changes changes_;
    std::unordered_set<ObjectKey, ObjectKeyHash> avatarKeys_;
    std::unordered_set<ObjectKey, ObjectKeyHash> animatedKeys_;
    // Keeps avatarKeys_ and animatedKeys_ right for an object added,
    // changed (null: removed).
    void classify(const ObjectKey& key, const Object* object)
    {
        if(object && object->type == ObjectType::Avatar)
        {
            avatarKeys_.insert(key);
        }
        else
        {
            avatarKeys_.erase(key);
        }
        if(object && object->type == ObjectType::Prim && object->animatedMesh)
        {
            animatedKeys_.insert(key);
        }
        else
        {
            animatedKeys_.erase(key);
        }
    }
    uint64_t serial_ = 0;
    void changed(const ObjectKey& key)
    {
        if(changes_.everything)
        {
            return;
        }
        if(changes_.keys.size() >= 65536)
        {
            changes_ = {{}, true};
            return;
        }
        changes_.keys.push_back(key);
    }
    std::unordered_map<Uuid, std::shared_ptr<const std::vector<PlayingAnimation>>> animations_;
    std::unordered_map<ObjectKey, std::shared_ptr<const FaceOverrides>, ObjectKeyHash>
        materialOverrides_;
    uint64_t objectsRevision_ = 0;
    uint64_t contentsRevision_ = 0;
    Stats stats_;
};

} // namespace viewer::world
