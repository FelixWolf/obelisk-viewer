#pragma once

// People: our friends and who's around us, as the official viewer keeps
// them (llcallingcard.cpp LLAvatarTracker, llpanelpeople.cpp):
// - Friends come with the login reply ("buddy-list": each one's id and the
//   rights each side gives the other). The region then says who of them is
//   online (OnlineNotification, OfflineNotification: only those letting us
//   see it), changes to rights (ChangeUserRights: ours, as the grid took
//   them, or theirs to us) and friendships ended by them
//   (TerminateFriendship). Friendships we form (accepting an offer, or
//   ours accepted: ImprovedInstantMessage dialog 39) start with both
//   seeing the other online.
// - Nearby is everyone the regions we're connected to report
//   (CoarseLocationUpdate, a few times a second each): their ids and
//   positions to the metre (height to 4 m).

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace viewer
{

namespace protocol::msg
{
struct CoarseLocationUpdate;
struct ChangeUserRights;
} // namespace protocol::msg

// What a friend may do (LLRelationship's GRANT_*).
namespace FriendRights
{
inline constexpr int32_t Online = 1;        // see that we're online
inline constexpr int32_t MapLocation = 2;   // find us on the map
inline constexpr int32_t ModifyObjects = 4; // edit, delete or take our objects
} // namespace FriendRights

struct Friend
{
    Uuid id;
    int32_t rightsGiven = 0; // what we let them do
    int32_t rightsHas = 0;   // what they let us do
    bool online = false;
};

class Friends
{
public:
    // The login reply's "buddy-list" (replaces what was kept).
    void load(const llsd::Value& buddyList);
    // Online and offline notifications.
    void setOnline(std::span<const Uuid> ids, bool online);
    // ChangeUserRights: rights we gave (AgentID is us), or theirs to us.
    void rightsChanged(const protocol::msg::ChangeUserRights& m, const Uuid& self);
    // What we now let a friend do (as sent: GrantUserRights).
    void setRightsGiven(const Uuid& id, int32_t rights);
    // A friendship formed (both seeing the other online), or ended.
    void form(const Uuid& id);
    void remove(const Uuid& id);

    const Friend* find(const Uuid& id) const;
    // In no particular order.
    const std::vector<Friend>& all() const { return friends_; }
    // Changes whenever anything kept does.
    uint64_t version() const { return version_; }
    // Logged out: nothing kept.
    void clear();

private:
    Friend* findMutable(const Uuid& id);

    std::vector<Friend> friends_;
    uint64_t version_ = 0;
};

class Nearby
{
public:
    struct Person
    {
        Uuid id;
        uint64_t regionHandle = 0;
        // In the region's coordinates; the height isn't known above
        // 1020 m (heightKnown false, the height 0 then).
        glm::vec3 position{0.0f};
        bool heightKnown = true;
    };

    // A region's CoarseLocationUpdate (replaces what it said before; we
    // aren't among them).
    void update(uint64_t regionHandle, const protocol::msg::CoarseLocationUpdate& m,
                const Uuid& self);
    // A region we're no longer connected to.
    void removeRegion(uint64_t regionHandle);

    // Everyone, by region.
    std::vector<Person> all() const;
    uint64_t version() const { return version_; }
    void clear();

private:
    std::unordered_map<uint64_t, std::vector<Person>> regions_;
    uint64_t version_ = 0;
};

// Where a position in a region is on the grid (metres; regions are 256 m
// apart, the handle holding the corner's x and y).
glm::dvec3 globalPosition(uint64_t regionHandle, const glm::vec3& local);

} // namespace viewer
