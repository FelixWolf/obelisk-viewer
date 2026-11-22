#include "InstantMessages.hpp"
#include "People.hpp"

#include "viewer/protocol/Messages.hpp"

#include <doctest/doctest.h>

using namespace viewer;
namespace msg = viewer::protocol::msg;

namespace
{

const Uuid kSelf = *Uuid::parse("11111111-1111-1111-1111-111111111111");
const Uuid kAnn = *Uuid::parse("22222222-2222-2222-2222-222222222222");
const Uuid kBob = *Uuid::parse("33333333-3333-3333-3333-333333333333");

llsd::Value buddy(const Uuid& id, int given, int has)
{
    llsd::Value b;
    b["buddy_id"] = id;
    b["buddy_rights_given"] = given;
    b["buddy_rights_has"] = has;
    return b;
}

} // namespace

TEST_CASE("Friends: the login's list, online status, rights, formed and ended")
{
    llsd::Value list;
    list.append(
        buddy(kAnn, FriendRights::Online | FriendRights::MapLocation, FriendRights::Online));
    list.append(buddy(kBob, FriendRights::Online, 0));
    list.append(buddy(kAnn, 0, 0)); // twice: once
    list.append(llsd::Value());     // no id: none
    Friends friends;
    friends.load(list);
    REQUIRE(friends.all().size() == 2);
    const Friend* ann = friends.find(kAnn);
    REQUIRE(ann);
    CHECK(ann->rightsGiven == (FriendRights::Online | FriendRights::MapLocation));
    CHECK(ann->rightsHas == FriendRights::Online);
    CHECK_FALSE(ann->online);

    // Online and offline; strangers ignored.
    const uint64_t before = friends.version();
    const Uuid online[] = {kAnn, kSelf};
    friends.setOnline(online, true);
    CHECK(friends.find(kAnn)->online);
    CHECK(friends.version() != before);
    friends.setOnline(std::span(online, 1), false);
    CHECK_FALSE(friends.find(kAnn)->online);

    // Rights: ours as the grid took them, and theirs to us.
    msg::ChangeUserRights ours;
    ours.AgentData.AgentID = kSelf;
    ours.Rights.push_back({kBob, FriendRights::Online | FriendRights::ModifyObjects});
    friends.rightsChanged(ours, kSelf);
    CHECK(friends.find(kBob)->rightsGiven == (FriendRights::Online | FriendRights::ModifyObjects));
    CHECK(friends.find(kBob)->rightsHas == 0);
    msg::ChangeUserRights theirs;
    theirs.AgentData.AgentID = kBob;
    theirs.Rights.push_back({kSelf, FriendRights::MapLocation});
    friends.rightsChanged(theirs, kSelf);
    CHECK(friends.find(kBob)->rightsHas == FriendRights::MapLocation);
    friends.setRightsGiven(kBob, 0);
    CHECK(friends.find(kBob)->rightsGiven == 0);

    // Ended, and formed again: both seeing the other online.
    friends.remove(kBob);
    CHECK_FALSE(friends.find(kBob));
    friends.form(kBob);
    REQUIRE(friends.find(kBob));
    CHECK(friends.find(kBob)->rightsGiven == FriendRights::Online);
    CHECK(friends.find(kBob)->rightsHas == FriendRights::Online);
    friends.form(kBob);
    CHECK(friends.all().size() == 2);

    friends.clear();
    CHECK(friends.all().empty());
}

TEST_CASE("Nearby: each region's people, without us; heights in 4 m steps")
{
    const uint64_t handle = (uint64_t{256000} << 32) | 256512;
    msg::CoarseLocationUpdate m;
    m.Location = {{10, 20, 5}, {30, 40, 255}, {1, 2, 3}};
    m.AgentData = {{kAnn}, {kBob}, {kSelf}};
    Nearby nearby;
    nearby.update(handle, m, kSelf);
    auto people = nearby.all();
    REQUIRE(people.size() == 2);
    CHECK(people[0].id == kAnn);
    CHECK(people[0].position == glm::vec3(10.0f, 20.0f, 20.0f));
    CHECK(people[0].heightKnown);
    CHECK_FALSE(people[1].heightKnown);

    // The same again: nothing changed.
    const uint64_t v = nearby.version();
    nearby.update(handle, m, kSelf);
    CHECK(nearby.version() == v);

    const glm::dvec3 g = globalPosition(handle, people[0].position);
    CHECK(g.x == doctest::Approx(256010.0));
    CHECK(g.y == doctest::Approx(256532.0));

    nearby.removeRegion(handle);
    CHECK(nearby.all().empty());
}

TEST_CASE("Instant messages: a conversation started from People, at the top")
{
    InstantMessages ims;
    const auto now = std::chrono::system_clock::now();
    const Uuid session = ims.start(kSelf, kAnn, "Ann", now);
    CHECK(session == directSession(kSelf, kAnn));
    REQUIRE(ims.find(session));
    CHECK(ims.find(session)->name == "Ann");
    CHECK(ims.find(session)->lines.empty());
    CHECK(ims.start(kSelf, kAnn, "Ann", now) == session);
    CHECK(ims.conversations().size() == 1);
}
