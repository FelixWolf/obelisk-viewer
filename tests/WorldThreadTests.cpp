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
