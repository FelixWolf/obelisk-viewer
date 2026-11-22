#include "ParcelInfo.hpp"

#include <array>
#include <utility>

namespace viewer
{

namespace
{

// A U32 sent as LLSD binary: four bytes, the most significant first.
uint32_t bigEndian(const llsd::Value& value)
{
    if(!value.get<llsd::Binary>())
    {
        return static_cast<uint32_t>(value.asInteger());
    }
    const llsd::Binary bytes = value.asBinary();
    uint32_t v = 0;
    for(size_t i = 0; i < bytes.size() && i < 4; i++)
    {
        v = v << 8 | bytes[i];
    }
    return v;
}

} // namespace

std::optional<Parcel> parseParcelProperties(const llsd::Value& body)
{
    const llsd::Value& d = body["ParcelData"][0];
    if(!d.isMap())
    {
        return std::nullopt;
    }
    Parcel p;
    p.localId = d["LocalID"].asInteger();
    if(const llsd::Value& e = body["ParcelEnvironmentBlock"][0]; e.isMap())
    {
        p.environmentVersion = e["ParcelEnvironmentVersion"].asInteger();
    }
    p.sequence = d["SequenceID"].asInteger();
    p.name = d["Name"].asString();
    p.description = d["Desc"].asString();
    p.owner = d["OwnerID"].asUuid();
    p.group = d["GroupID"].asUuid();
    p.groupOwned = d["IsGroupOwned"].asBoolean();
    p.area = d["Area"].asInteger();
    p.salePrice = d["SalePrice"].asInteger();
    p.flags = bigEndian(d["ParcelFlags"]);
    p.category = d["Category"].asInteger();
    p.prims = d["TotalPrims"].asInteger();
    p.maxPrims = d["MaxPrims"].asInteger();
    p.snapshot = d["SnapshotID"].asUuid();
    p.musicUrl = d["MusicURL"].asString();
    p.mediaUrl = d["MediaURL"].asString();
    p.seeAvatars = !d.has("SeeAVs") || d["SeeAVs"].asBoolean();
    return p;
}

std::string_view parcelCategoryName(int category)
{
    static constexpr std::array<std::string_view, 15> kNames = {"",
                                                                "Linden location",
                                                                "Adult",
                                                                "Arts and culture",
                                                                "Business",
                                                                "Education",
                                                                "Gaming",
                                                                "Hangout",
                                                                "Newcomer friendly",
                                                                "Parks and nature",
                                                                "Residential",
                                                                "Shopping",
                                                                "Stage",
                                                                "Other",
                                                                "Rental"};
    return category >= 0 && category < static_cast<int>(kNames.size())
               ? kNames[static_cast<size_t>(category)]
               : std::string_view();
}

void ParcelInfo::parcelProperties(uint64_t region, uint64_t agentRegion, const llsd::Value& body)
{
    std::optional<Parcel> p = parseParcelProperties(body);
    if(!p || region != agentRegion || p->sequence < 0)
    {
        return;
    }
    // Out of order (an older one for the same region): kept out.
    if(parcel_ && region_ == region && p->sequence != 0 && p->sequence < parcel_->sequence)
    {
        return;
    }
    if(region != estateRegion_)
    {
        estateRegion_ = region;
        estate_.reset();
        estateWanted_ = true;
    }
    parcel_ = std::move(*p);
    region_ = region;
    version_++;
}

void ParcelInfo::groupName(const Uuid& group, std::string name)
{
    groupsWanted_.erase(group);
    std::string& known = groups_[group];
    if(known != name)
    {
        known = std::move(name);
        version_++;
    }
}

void ParcelInfo::estate(std::string name, const Uuid& owner, const Uuid& covenant)
{
    estate_ = Estate{std::move(name), owner, covenant};
    version_++;
}

const std::string& ParcelInfo::group(const Uuid& id)
{
    const auto [it, added] = groups_.try_emplace(id);
    if(added && !id.isNull())
    {
        groupsWanted_.insert(id);
    }
    return it->second;
}

ParcelInfo::Requests ParcelInfo::requests()
{
    Requests r;
    r.groups.assign(groupsWanted_.begin(), groupsWanted_.end());
    groupsWanted_.clear();
    r.estate = std::exchange(estateWanted_, false);
    return r;
}

void ParcelInfo::clear()
{
    parcel_.reset();
    region_ = 0;
    estate_.reset();
    estateRegion_ = 0;
    estateWanted_ = false;
    groups_.clear();
    groupsWanted_.clear();
    version_++;
}

} // namespace viewer
