#include "TestUtil.hpp"

#include "viewer/protocol/Messages.hpp"
#include "viewer/world/World.hpp"
#include "viewer/world/WorldThread.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <format>
#include <memory>

using namespace viewer;
namespace msg = viewer::protocol::msg;

namespace
{

template <typename M>
protocol::CircuitEvent event(const M& message)
{
    protocol::CircuitEvent e;
    e.messageId = M::kInfo.id;
    e.message = protocol::encodeMessage(message);
    return e;
}

msg::ObjectUpdate objectUpdate(uint32_t localId, float x)
{
    msg::ObjectUpdate m;
    msg::ObjectUpdate::ObjectDataBlock b;
    b.ID = localId;
    b.FullID = *Uuid::parse(std::format("00000000-0000-0000-0000-{:012x}", localId));
    b.PCode = 9; // prim
    b.Scale = {1.0f, 1.0f, 1.0f};
    b.ObjectData.resize(60, 0);
    std::memcpy(b.ObjectData.data(), &x, sizeof(x));
    m.ObjectData.push_back(b);
    return m;
}

} // namespace

TEST_CASE("World thread: applies messages in order and publishes snapshots")
{
    constexpr uint64_t kRegion = (256000ull << 32) | 256000ull;
    world::WorldThread thread;
    const auto empty = thread.snapshot();
    REQUIRE(empty != nullptr);
    CHECK(empty->objectCount() == 0);

    thread.post(kRegion, event(objectUpdate(1, 10.0f)));
    thread.post(kRegion, event(objectUpdate(2, 20.0f)));
    thread.post(kRegion, event(objectUpdate(1, 11.0f))); // a later update wins
    msg::KillObject kill;
    kill.ObjectData.push_back({2});
    thread.post(kRegion, event(kill));
    msg::AvatarAnimation animation;
    animation.Sender.ID = *Uuid::parse("00000000-0000-0000-0000-0000000000aa");
    animation.AnimationList.push_back({*Uuid::parse("00000000-0000-0000-0000-0000000000bb"), 7});
    thread.post(kRegion, event(animation));
    thread.post(kRegion, protocol::CircuitEvent{}); // nothing for the world
    thread.flush();

    const auto first = thread.snapshot();
    REQUIRE(first->object(kRegion, 1) != nullptr);
    CHECK(first->object(kRegion, 1)->position.x == doctest::Approx(11.0f));
    CHECK(first->object(kRegion, 2) == nullptr);
    CHECK(first->stats().fullUpdates == 3);
    const auto* playing = first->animations(animation.Sender.ID);
    REQUIRE(playing != nullptr);
    REQUIRE(playing->size() == 1);
    CHECK((*playing)[0].sequence == 7);
    CHECK(empty->objectCount() == 0); // earlier snapshots don't change

    // Later changes leave a snapshot alone: objects are copied on write.
    thread.post(kRegion, event(objectUpdate(1, 12.0f)));
    thread.post(kRegion, event(objectUpdate(4, 40.0f)));
    thread.flush();
    const auto second = thread.snapshot();
    CHECK(first->object(kRegion, 1)->position.x == doctest::Approx(11.0f));
    CHECK(first->object(kRegion, 4) == nullptr);
    CHECK(second->object(kRegion, 1)->position.x == doctest::Approx(12.0f));
    CHECK(second->objectCount() == 2);

    // Removing a region is ordered with the messages around it.
    thread.retire(first);
    thread.postRegionRemoved(kRegion);
    thread.post(kRegion, event(objectUpdate(3, 30.0f)));
    thread.flush();
    const auto third = thread.snapshot();
    CHECK(third->object(kRegion, 1) == nullptr);
    CHECK(third->object(kRegion, 3) != nullptr);
    CHECK(second->objectCount() == 2);

    // Destroying it with work queued doesn't hang.
    auto busy = std::make_unique<world::WorldThread>();
    for(uint32_t i = 0; i < 1000; i++)
    {
        busy->post(kRegion, event(objectUpdate(i, 1.0f)));
    }
    busy.reset();
}

namespace
{

world::WorldUpdate fullUpdate(uint64_t region, uint32_t localId, const Uuid& id,
                              uint32_t parentId = 0)
{
    world::ObjectUpdateData u;
    u.localId = localId;
    u.full.id = id;
    u.full.localId = localId;
    u.full.parentId = parentId;
    u.full.type = world::ObjectType::Avatar;
    world::WorldUpdate::Objects objects;
    objects.updates.push_back(u);
    return world::WorldUpdate{.region = region, .change = std::move(objects)};
}

} // namespace

TEST_CASE("World: an object moving to another region leaves no copy behind")
{
    const uint64_t west = 1000ull * 256 << 32 | 1000 * 256;
    const uint64_t east = 1001ull * 256 << 32 | 1000 * 256;
    const Uuid avatar = *Uuid::parse("6f1cbf7d-7945-47cb-8428-8cc14a60e94c");
    const Uuid hair = *Uuid::parse("00000000-0000-0000-0000-000000000001");
    const Uuid hairPart = *Uuid::parse("00000000-0000-0000-0000-000000000002");
    const Uuid other = *Uuid::parse("00000000-0000-0000-0000-000000000003");

    world::World world;
    world.apply(fullUpdate(west, 10, avatar));
    world.apply(fullUpdate(west, 11, hair, 10));     // attached to the avatar
    world.apply(fullUpdate(west, 12, hairPart, 11)); // linked to the attachment
    world.apply(fullUpdate(west, 13, other));
    const world::World snapshot = world; // copies don't disturb the original
    CHECK(snapshot.objectCount() == 4);

    // It walks east: the east region sends it; the west one never kills it.
    world.apply(fullUpdate(east, 50, avatar));
    CHECK(world.object(west, 10) == nullptr);
    CHECK(world.object(west, 11) == nullptr);
    CHECK(world.object(west, 12) == nullptr);
    CHECK(world.object(west, 13) != nullptr);
    CHECK(world.object(east, 50) != nullptr);
    CHECK(world.objectCount() == 2);
    CHECK(snapshot.objectCount() == 4);

    // A copy that gets updated rebuilds its index first.
    world::World copy = snapshot;
    copy.apply(fullUpdate(east, 50, avatar));
    CHECK(copy.objectCount() == 2);

    // Killed and sent again: no stale index entry removes anything.
    world.apply(world::WorldUpdate{.region = east, .change = world::WorldUpdate::Kill{{50}}});
    world.apply(fullUpdate(west, 20, avatar));
    CHECK(world.objectCount() == 2);
    CHECK(world.object(west, 20) != nullptr);
}

TEST_CASE("World: objects remember when their update arrived")
{
    const uint64_t region = 1000ull * 256 << 32 | 1000 * 256;
    const Uuid id = *Uuid::parse("00000000-0000-0000-0000-000000000009");
    const auto arrived = std::chrono::steady_clock::now();

    world::World world;
    world::WorldUpdate full = fullUpdate(region, 7, id);
    auto& objects = std::get<world::WorldUpdate::Objects>(full.change);
    objects.received = arrived;
    objects.timeDilation = 0.5f;
    world.apply(std::move(full));
    REQUIRE(world.object(region, 7) != nullptr);
    CHECK(world.object(region, 7)->updatedAt == arrived);
    CHECK(world.object(region, 7)->timeDilation == 0.5f);

    // Terse updates restamp it.
    world::ObjectUpdateData terse;
    terse.kind = world::ObjectUpdateData::Kind::Terse;
    terse.localId = 7;
    terse.velocity = glm::vec3(1.0f, 0.0f, 0.0f);
    world::WorldUpdate::Objects moved;
    moved.updates.push_back(terse);
    moved.received = arrived + std::chrono::seconds(1);
    world.apply(world::WorldUpdate{.region = region, .change = std::move(moved)});
    CHECK(world.object(region, 7)->updatedAt == arrived + std::chrono::seconds(1));
    CHECK(world.object(region, 7)->timeDilation == 1.0f);
    CHECK(world.object(region, 7)->velocity.x == 1.0f);
}

namespace
{

// A minimal ObjectUpdateCompressed block: not a prim (no shape), no
// optional parts.
std::vector<uint8_t> compressedBlock(uint32_t localId, uint32_t crc, float x)
{
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v)
    {
        for(int i = 0; i < 4; i++)
        {
            b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    };
    auto f32 = [&](float v) { u32(std::bit_cast<uint32_t>(v)); };
    const Uuid id = *Uuid::parse(std::format("00000000-0000-0000-0001-{:012x}", localId));
    b.insert(b.end(), id.bytes.begin(), id.bytes.end());
    u32(localId);
    b.push_back(255); // tree
    b.push_back(0);   // state
    u32(crc);
    b.push_back(0); // material
    b.push_back(0); // click action
    for(const float v : {1.0f, 1.0f, 1.0f, x, 20.0f, 30.0f, 0.0f, 0.0f, 0.0f})
    {
        f32(v); // scale, position, rotation
    }
    u32(0);                   // flags
    b.insert(b.end(), 16, 0); // owner
    b.push_back(0);           // no extra parameters
    return b;
}

msg::RegionHandshake regionHandshake(const Uuid& regionId, const Uuid& cacheId)
{
    msg::RegionHandshake m;
    m.RegionInfo2.RegionID = regionId;
    m.RegionInfo.CacheID = cacheId;
    return m;
}

msg::ObjectUpdateCompressed
compressedUpdate(std::initializer_list<std::pair<uint32_t, uint32_t>> objects, uint32_t flags = 0)
{
    msg::ObjectUpdateCompressed m;
    for(const auto& [localId, crc] : objects)
    {
        auto& block = m.ObjectData.emplace_back();
        block.UpdateFlags = flags;
        block.Data = compressedBlock(localId, crc, static_cast<float>(localId));
    }
    return m;
}

msg::ObjectUpdateCached probes(std::initializer_list<std::pair<uint32_t, uint32_t>> objects)
{
    msg::ObjectUpdateCached m;
    for(const auto& [localId, crc] : objects)
    {
        auto& block = m.ObjectData.emplace_back();
        block.ID = localId;
        block.CRC = crc;
    }
    return m;
}

std::vector<world::WorldRequest> requestsAfterFlush(world::WorldThread& thread)
{
    thread.flush();
    std::vector<world::WorldRequest> out;
    thread.drainRequests(out);
    return out;
}

} // namespace

TEST_CASE("Object cache: file format round trip, and the region instance must match")
{
    const Uuid cacheId = *Uuid::parse("aaaaaaaa-0000-0000-0000-000000000001");
    world::RegionObjectCache cache(cacheId);
    CHECK_FALSE(cache.dirty());
    cache.store(5, {111, 0x10, {1, 2, 3}});
    cache.store(6, {222, 0, {4}});
    CHECK(cache.dirty());
    cache.erase(6);

    const auto bytes = cache.serialize();
    const auto loaded = world::RegionObjectCache::parse(bytes, cacheId);
    REQUIRE(loaded.has_value());
    CHECK(loaded->size() == 1);
    CHECK_FALSE(loaded->dirty());
    REQUIRE(loaded->find(5, 111) != nullptr);
    CHECK(loaded->find(5, 111)->data == std::vector<uint8_t>{1, 2, 3});
    CHECK(loaded->find(5, 111)->updateFlags == 0x10);
    CHECK(loaded->find(5, 112) == nullptr); // changed since
    CHECK(loaded->contains(5));

    // Another instance of the region, or damage: nothing.
    CHECK_FALSE(world::RegionObjectCache::parse(
        bytes, *Uuid::parse("aaaaaaaa-0000-0000-0000-000000000002")));
    auto truncated = bytes;
    truncated.pop_back();
    CHECK_FALSE(world::RegionObjectCache::parse(truncated, cacheId));
    CHECK_FALSE(world::RegionObjectCache::parse({}, cacheId));
}

TEST_CASE("World thread: objects come from the cache on the next visit")
{
    viewer::test::TempDirectory dir("object-cache");
    FileCache store(dir.path());
    const uint64_t region = 1000ull * 256 << 32 | 1000 * 256;
    const Uuid regionId = *Uuid::parse("bbbbbbbb-0000-0000-0000-000000000001");
    const Uuid cacheId = *Uuid::parse("cccccccc-0000-0000-0000-000000000001");

    {
        // First visit: nothing cached, so the region should send everything.
        world::WorldThread thread(&store);
        thread.post(region, event(regionHandshake(regionId, cacheId)));
        auto requests = requestsAfterFlush(thread);
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].kind == world::WorldRequest::Kind::HandshakeReply);
        CHECK(requests[0].region == region);
        CHECK_FALSE(requests[0].haveCache);

        thread.post(region, event(compressedUpdate({{10, 100}, {11, 110}, {12, 120}})));
        thread.post(region, event(compressedUpdate({{13, 130}}, world::kFlagTemporaryOnRez)));
        // A terse update: it moves, so it's no longer worth caching.
        msg::ImprovedTerseObjectUpdate terse;
        auto& block = terse.ObjectData.emplace_back();
        block.Data.resize(44, 0);
        block.Data[0] = 12; // local id 12
        thread.post(region, event(terse));
        thread.flush();
        CHECK(thread.snapshot()->objectCount() == 4);
        thread.postRegionRemoved(region); // saves it
        thread.flush();
    }
    CHECK(store.contains({CacheKind::Objects, regionId}));

    world::WorldThread thread(&store);
    thread.post(region, event(regionHandshake(regionId, cacheId)));
    auto requests = requestsAfterFlush(thread);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].haveCache);

    // Probes: 10 matches, 11 changed, 12 and 13 were never kept, 14 is new.
    thread.post(region, event(probes({{10, 100}, {11, 999}, {12, 120}, {13, 130}, {14, 140}})));
    requests = requestsAfterFlush(thread);
    const auto world = thread.snapshot();
    REQUIRE(world->object(region, 10) != nullptr);
    CHECK(world->object(region, 10)->position.x == 10.0f);
    CHECK(world->objectCount() == 1);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == world::WorldRequest::Kind::SendObjects);
    REQUIRE(requests[0].objects.size() == 4);
    CHECK(requests[0].objects[0].localId == 11);
    CHECK(requests[0].objects[0].miss == world::WorldRequest::Miss::Crc);
    CHECK(requests[0].objects[1].localId == 12);
    CHECK(requests[0].objects[1].miss == world::WorldRequest::Miss::Total);
    CHECK(requests[0].objects[3].localId == 14);
    CHECK(thread.cacheStats().hits == 1);
    CHECK(thread.cacheStats().misses == 4);

    // A region restart (new CacheID) makes the old cache worthless.
    world::WorldThread restarted(&store);
    restarted.post(region, event(regionHandshake(
                               regionId, *Uuid::parse("cccccccc-0000-0000-0000-000000000002"))));
    requests = requestsAfterFlush(restarted);
    REQUIRE(requests.size() == 1);
    CHECK_FALSE(requests[0].haveCache);
}

namespace
{

msg::GenericStreamingMessage materialOverride(std::string_view notation)
{
    msg::GenericStreamingMessage m;
    m.MethodData.Method = 0x4175;
    m.DataBlock.Data.assign(notation.begin(), notation.end());
    return m;
}

} // namespace

TEST_CASE("World thread: PBR material overrides, streamed and cached")
{
    viewer::test::TempDirectory dir("material-overrides");
    FileCache store(dir.path());
    const uint64_t region = 1000ull * 256 << 32 | 1000 * 256;
    const Uuid regionId = *Uuid::parse("bbbbbbbb-0000-0000-0000-000000000002");
    const Uuid cacheId = *Uuid::parse("cccccccc-0000-0000-0000-000000000003");
    const std::string notation = "{'id':i10,'te':[i2],'od':[{'mf':r0.25,'am':i1}]}";

    {
        world::WorldThread thread(&store);
        thread.post(region, event(regionHandshake(regionId, cacheId)));
        // The override may come before the object: it's kept.
        thread.post(region, event(materialOverride(notation)));
        thread.post(region, event(compressedUpdate({{10, 100}, {11, 110}})));
        thread.flush();
        const auto world = thread.snapshot();
        const world::FaceOverrides* o = world->materialOverrides({region, 10});
        REQUIRE(o != nullptr);
        REQUIRE(o->size() == 1);
        CHECK((*o)[0].first == 2);
        CHECK((*o)[0].second.metallic == 0.25f);
        CHECK((*o)[0].second.alphaMode == viewer::assets::MaterialAlpha::Blend);
        CHECK(world->materialOverrides({region, 11}) == nullptr);
        thread.postRegionRemoved(region); // saves the cache
        thread.flush();
    }

    // Next visit: the object comes from the cache, and so does its override
    // (the simulator doesn't stream it again).
    world::WorldThread thread(&store);
    thread.post(region, event(regionHandshake(regionId, cacheId)));
    thread.post(region, event(probes({{10, 100}})));
    thread.flush();
    auto world = thread.snapshot();
    REQUIRE(world->object(region, 10) != nullptr);
    REQUIRE(world->materialOverrides({region, 10}) != nullptr);
    CHECK((*world->materialOverrides({region, 10}))[0].second.metallic == 0.25f);

    // An empty set clears them; so does a kill.
    thread.post(region, event(materialOverride("{'id':i10,'te':[],'od':[]}")));
    thread.flush();
    CHECK(thread.snapshot()->materialOverrides({region, 10}) == nullptr);
    thread.post(region, event(materialOverride(notation)));
    msg::KillObject kill;
    kill.ObjectData.emplace_back().ID = 10;
    thread.post(region, event(kill));
    thread.flush();
    CHECK(thread.snapshot()->materialOverrides({region, 10}) == nullptr);
}
