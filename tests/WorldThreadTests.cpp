#include "viewer/protocol/Messages.hpp"
#include "viewer/world/World.hpp"
#include "viewer/world/WorldThread.hpp"

#include <doctest/doctest.h>

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
