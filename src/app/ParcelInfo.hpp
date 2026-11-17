#pragma once

// The parcel we're on (the main menu's Parcel tab): its properties, which
// the region sends on its event queue whenever we come onto one
// (ParcelProperties, sequence 0 or up; negative ones answer requests for
// others, which we don't make), the names of the groups it's set to or
// owned by (UUIDGroupNameRequest), and its region's estate (its name and
// owner: EstateCovenantRequest, asked for once a region).
//
// No messages of its own: the app sends what requests() says and gives it
// the answers. Per session (clear() between).

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

// ParcelFlags (llparcelflags.h's PF_*), those shown.
namespace parcel_flags
{
inline constexpr uint32_t kAllowFly = 1u << 0;
inline constexpr uint32_t kAllowOtherScripts = 1u << 1;
inline constexpr uint32_t kForSale = 1u << 2;
inline constexpr uint32_t kAllowTerraform = 1u << 4;
inline constexpr uint32_t kAllowDamage = 1u << 5;
inline constexpr uint32_t kCreateObjects = 1u << 6;
inline constexpr uint32_t kUseAccessGroup = 1u << 8;
inline constexpr uint32_t kUseAccessList = 1u << 9;
inline constexpr uint32_t kSoundLocal = 1u << 15;
inline constexpr uint32_t kRestrictPush = 1u << 21;
inline constexpr uint32_t kAllowGroupScripts = 1u << 25;
inline constexpr uint32_t kCreateGroupObjects = 1u << 26;
inline constexpr uint32_t kAllowVoice = 1u << 29;
} // namespace parcel_flags

struct Parcel
{
    int localId = 0;
    int sequence = 0;
    std::string name;
    std::string description;
    Uuid owner;
    Uuid group;
    bool groupOwned = false;
    int area = 0;      // square meters
    int salePrice = 0; // L$, when for sale
    uint32_t flags = 0;
    int category = 0; // llparcel.h's C_*
    int prims = 0;    // on it
    int maxPrims = 0; // it can have
    Uuid snapshot;    // its picture (a texture)
    std::string musicUrl;
    std::string mediaUrl;
    bool seeAvatars = true; // from outside it

    bool has(uint32_t flag) const { return (flags & flag) != 0; }
};

// ParcelProperties' body; nothing if it has no parcel in it.
std::optional<Parcel> parseParcelProperties(const llsd::Value& body);
// A category's name ("" for none).
std::string_view parcelCategoryName(int category);

class ParcelInfo
{
public:
    // ParcelProperties from a region: the agent's, if it's from the region
    // the agent's in (agentRegion).
    void parcelProperties(uint64_t region, uint64_t agentRegion, const llsd::Value& body);
    // UUIDGroupNameReply.
    void groupName(const Uuid& group, std::string name);
    // EstateCovenantReply.
    void estate(std::string name, const Uuid& owner, const Uuid& covenant);

    // The parcel we're on; nothing until the region says.
    const std::optional<Parcel>& parcel() const { return parcel_; }
    uint64_t region() const { return region_; }
    // A group's name; empty until known (it's asked for: requests()).
    const std::string& group(const Uuid& id);

    struct Estate
    {
        std::string name;
        Uuid owner;
        Uuid covenant; // a notecard; null: none
    };
    const std::optional<Estate>& estate() const { return estate_; }

    // What to send now: groups' names to ask, and whether to ask the
    // region for its estate.
    struct Requests
    {
        std::vector<Uuid> groups;
        bool estate = false;
    };
    Requests requests();

    // Changes when anything shown might have.
    uint64_t version() const { return version_; }
    void clear();

private:
    std::optional<Parcel> parcel_;
    uint64_t region_ = 0;
    std::optional<Estate> estate_;
    uint64_t estateRegion_ = 0; // the region estate_ is (or is being asked) for
    bool estateWanted_ = false;
    std::map<Uuid, std::string> groups_; // empty: asked for
    std::set<Uuid> groupsWanted_;
    uint64_t version_ = 0;
};

} // namespace viewer
