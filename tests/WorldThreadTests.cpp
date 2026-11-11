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

TEST_CASE("World thread: decodes in order, World applies")
{
    constexpr uint64_t kRegion = (256000ull << 32) | 256000ull;
    world::WorldThread thread;
    world::World world;

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

    std::vector<world::WorldUpdate> updates;
    thread.drain(updates);
    CHECK(updates.size() == 5); // the empty message produced nothing
    for(auto& u : updates)
    {
        world.apply(std::move(u));
    }
    REQUIRE(world.object(kRegion, 1) != nullptr);
    CHECK(world.object(kRegion, 1)->position.x == doctest::Approx(11.0f));
    CHECK(world.object(kRegion, 2) == nullptr);
    CHECK(world.stats().fullUpdates == 3);
    const auto* playing = world.animations(animation.Sender.ID);
    REQUIRE(playing != nullptr);
    REQUIRE(playing->size() == 1);
    CHECK((*playing)[0].sequence == 7);

    // Removing a region is ordered with the messages around it.
    thread.postRegionRemoved(kRegion);
    thread.post(kRegion, event(objectUpdate(3, 30.0f)));
    thread.flush();
    thread.drain(updates);
    for(auto& u : updates)
    {
        world.apply(std::move(u));
    }
    CHECK(world.object(kRegion, 1) == nullptr);
    CHECK(world.object(kRegion, 3) != nullptr);

    // Destroying it with work queued doesn't hang.
    auto busy = std::make_unique<world::WorldThread>();
    for(uint32_t i = 0; i < 1000; i++)
    {
        busy->post(kRegion, event(objectUpdate(i, 1.0f)));
    }
    busy.reset();
}
