#include "ObjectIndex.hpp"

#include <doctest/doctest.h>

using namespace viewer;

namespace
{

constexpr uint64_t kRegion = 1000ull * 256 << 32 | 1000 * 256;

world::WorldUpdate prim(uint32_t localId, uint32_t parentId, glm::vec3 position,
                        glm::vec3 velocity = glm::vec3(0.0f),
                        uint8_t type = world::ObjectType::Prim)
{
    world::ObjectUpdateData u;
    u.localId = localId;
    u.full.id.bytes[15] = static_cast<uint8_t>(localId);
    u.full.id.bytes[14] = static_cast<uint8_t>(localId >> 8);
    u.full.localId = localId;
    u.full.parentId = parentId;
    u.full.type = type;
    u.full.position = position;
    u.full.velocity = velocity;
    u.full.scale = glm::vec3(1.0f);
    world::WorldUpdate::Objects objects;
    objects.updates.push_back(u);
    return world::WorldUpdate{.region = kRegion, .change = std::move(objects)};
}

world::WorldUpdate kill(uint32_t localId)
{
    return world::WorldUpdate{.region = kRegion, .change = world::WorldUpdate::Kill{{localId}}};
}

} // namespace

TEST_CASE("Object index: link sets in cells, kept up from each snapshot's changes")
{
    world::World w;
    w.apply(prim(1, 0, glm::vec3(10.0f, 10.0f, 20.0f)));
    w.apply(prim(2, 1, glm::vec3(0.0f, 0.0f, 2.0f))); // a child, 2 m up
    w.apply(prim(3, 0, glm::vec3(200.0f, 200.0f, 20.0f), glm::vec3(1.0f, 0.0f, 0.0f))); // moving
    ObjectIndex index;
    index.update(w); // first: everything
    const world::ObjectKey root{kRegion, 1};
    const ObjectIndex::LinkSet* set = index.linkSet(root);
    REQUIRE(set);
    CHECK(set->members.size() == 2);
    CHECK_FALSE(set->moving);
    CHECK(set->center.x == doctest::Approx(256000.0 + 10.0));
    // Its sphere reaches the child (2 m up) and past it.
    CHECK(set->radius >= 2.0f);
    CHECK(index.cells().size() == 1);
    CHECK(index.moving().count(world::ObjectKey{kRegion, 3}) == 1);
    CHECK(index.prims() == 3);

    // The next snapshot: the child goes, a prim comes.
    w.startNextSnapshot();
    w.apply(kill(2));
    w.apply(prim(4, 0, glm::vec3(100.0f, 10.0f, 20.0f)));
    index.update(w);
    REQUIRE(index.linkSet(root));
    CHECK(index.linkSet(root)->members.size() == 1);
    CHECK(index.cells().size() == 2);
    CHECK(index.prims() == 3);

    // The moving one stops: into a cell.
    w.startNextSnapshot();
    w.apply(prim(3, 0, glm::vec3(200.0f, 200.0f, 20.0f)));
    index.update(w);
    CHECK(index.moving().empty());
    CHECK(index.cells().size() == 3);

    // A child that came before its parent waits apart, then joins it.
    w.startNextSnapshot();
    w.apply(prim(6, 5, glm::vec3(0.0f, 1.0f, 0.0f)));
    index.update(w);
    CHECK(index.moving().count(world::ObjectKey{kRegion, 6}) == 1);
    w.startNextSnapshot();
    w.apply(prim(5, 0, glm::vec3(50.0f, 50.0f, 20.0f)));
    index.update(w);
    REQUIRE(index.linkSet(world::ObjectKey{kRegion, 5}));
    CHECK(index.linkSet(world::ObjectKey{kRegion, 5})->members.size() == 2);
    CHECK(index.moving().empty());

    // A snapshot missed: everything again, the same answer.
    w.startNextSnapshot();
    w.startNextSnapshot();
    w.apply(kill(4));
    index.update(w);
    CHECK(index.prims() == 4);
    CHECK_FALSE(index.linkSet(world::ObjectKey{kRegion, 4}));
}

TEST_CASE("Object index: the ground under a point, from still prims' boxes")
{
    world::World w;
    w.apply(prim(1, 0, glm::vec3(10.0f, 10.0f, 20.0f))); // a 1 m cube, top at 20.5
    w.apply(prim(2, 0, glm::vec3(10.0f, 10.0f, 19.0f))); // under it
    w.apply(prim(3, 0, glm::vec3(30.0f, 30.0f, 20.0f), glm::vec3(1.0f, 0.0f, 0.0f))); // moving
    ObjectIndex index;
    index.update(w);
    const glm::dvec3 corner(1000.0 * 256, 1000.0 * 256, 0.0);

    // From just above: the top one's top.
    const auto hit = index.groundBelow(corner + glm::dvec3(10.2, 9.8, 21.0), 1.0);
    REQUIRE(hit);
    CHECK(hit->height == doctest::Approx(20.5));
    CHECK(hit->prim.localId == 1);
    // Out of reach, off to the side, and moving prims: nothing.
    CHECK_FALSE(index.groundBelow(corner + glm::dvec3(10.0, 10.0, 22.0), 1.0));
    CHECK_FALSE(index.groundBelow(corner + glm::dvec3(12.0, 10.0, 21.0), 1.0));
    CHECK_FALSE(index.groundBelow(corner + glm::dvec3(30.0, 30.0, 21.0), 1.0));
}
