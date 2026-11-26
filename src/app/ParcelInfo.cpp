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
    p.media.url = d["MediaURL"].asString();
    p.media.texture = d["MediaID"].asUuid();
    p.media.autoScale = d["MediaAutoScale"].asInteger() != 0;
    if(const llsd::Value& m = body["MediaData"][0]; m.isMap())
    {
        p.media.type = m["MediaType"].asString();
        p.media.description = m["MediaDesc"].asString();
        p.media.width = m["MediaWidth"].asInteger();
        p.media.height = m["MediaHeight"].asInteger();
        p.media.loop = m["MediaLoop"].asInteger() != 0;
    }
    p.seeAvatars = !d.has("SeeAVs") || d["SeeAVs"].asBoolean();
    if(d["Bitmap"].get<llsd::Binary>())
    {
        const llsd::Binary bitmap = d["Bitmap"].asBinary();
        p.bitmap.assign(bitmap.begin(), bitmap.end());
    }
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
    if(!parcel_ || parcel_->localId != p->localId || region_ != region)
    {
        // A new parcel: no script has said anything about its media yet.
        mediaPlay_ = MediaPlay{std::nullopt, std::nullopt, mediaPlay_.serial + 1};
    }
    parcel_ = std::move(*p);
    region_ = region;
    version_++;
}

void ParcelInfo::mediaUpdate(ParcelMedia media)
{
    if(parcel_ && parcel_->media != media)
    {
        parcel_->media = std::move(media);
        version_++;
    }
}

void ParcelInfo::mediaCommand(uint32_t flags, uint32_t command, float time)
{
    namespace c = parcel_media_command;
    constexpr uint32_t kPlayState =
        1u << c::kStop | 1u << c::kPause | 1u << c::kPlay | 1u << c::kLoop | 1u << c::kUnload;
    if(flags & kPlayState)
    {
        mediaPlay_.command = command == c::kUnload ? c::kStop : command;
    }
    if(flags & (1u << c::kTime))
    {
        mediaPlay_.time = time;
    }
    mediaPlay_.serial++;
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
    mediaPlay_ = MediaPlay{};
    estate_.reset();
    estateRegion_ = 0;
    estateWanted_ = false;
    groups_.clear();
    groupsWanted_.clear();
    version_++;
}

} // namespace viewer
