#include "ParcelInfo.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <format>

using namespace viewer;

namespace
{

// A ParcelProperties body as a region sends it (trimmed).
llsd::Value parcelProperties(int sequence, const char* name)
{
    const auto body = llsd::fromXml(std::format(
        R"(<llsd><map><key>ParcelData</key><array><map>
        <key>Area</key><integer>7248</integer>
        <key>Category</key><integer>11</integer>
        <key>Desc</key><string>The oldest sandbox.</string>
        <key>GroupID</key><uuid>779c87b7-89cd-cba1-858f-673adb2b7b37</uuid>
        <key>IsGroupOwned</key><boolean>true</boolean>
        <key>LocalID</key><integer>3</integer>
        <key>MaxPrims</key><integer>2489</integer>
        <key>MusicURL</key><string>http://example.com/stream</string>
        <key>Name</key><string>{}</string>
        <key>OwnerID</key><uuid>779c87b7-89cd-cba1-858f-673adb2b7b37</uuid>
        <key>ParcelFlags</key><binary>NiCUCw==</binary>
        <key>SalePrice</key><integer>165000</integer>
        <key>SeeAVs</key><boolean>true</boolean>
        <key>SequenceID</key><integer>{}</integer>
        <key>SnapshotID</key><uuid>c1fd2f59-5828-a3cd-3989-6609b6465158</uuid>
        <key>TotalPrims</key><integer>97</integer>
        </map></array></map></llsd>)",
        name, sequence));
    REQUIRE(body);
    return *body;
}

} // namespace

TEST_CASE("Parcel info: the parcel's properties, as a region sends them")
{
    const auto p = parseParcelProperties(parcelProperties(0, "PUBLIC SANDBOX"));
    REQUIRE(p);
    CHECK(p->name == "PUBLIC SANDBOX");
    CHECK(p->description == "The oldest sandbox.");
    CHECK(p->localId == 3);
    CHECK(p->area == 7248);
    CHECK(p->groupOwned);
    CHECK(p->owner.toString() == "779c87b7-89cd-cba1-858f-673adb2b7b37");
    CHECK(p->group == p->owner);
    CHECK(p->prims == 97);
    CHECK(p->maxPrims == 2489);
    CHECK(p->salePrice == 165000);
    CHECK(p->snapshot.toString() == "c1fd2f59-5828-a3cd-3989-6609b6465158");
    CHECK(p->musicUrl == "http://example.com/stream");
    CHECK(parcelCategoryName(p->category) == "Shopping");
    // Its flags, four bytes, most significant first: 0x3620940b.
    CHECK(p->flags == 0x3620940bu);
    CHECK(p->has(parcel_flags::kAllowFly));
    CHECK(p->has(parcel_flags::kAllowOtherScripts));
    CHECK_FALSE(p->has(parcel_flags::kForSale));
    CHECK(p->has(parcel_flags::kAllowVoice));
    CHECK_FALSE(parseParcelProperties(llsd::Value()));
}

TEST_CASE("Parcel info: the agent's parcel, its groups' names and its estate")
{
    ParcelInfo info;
    // Another region's, or an answer to a request for another parcel
    // (negative sequence): not the agent's.
    info.parcelProperties(2, 1, parcelProperties(0, "Elsewhere"));
    info.parcelProperties(1, 1, parcelProperties(-10000, "Selected"));
    CHECK_FALSE(info.parcel());

    info.parcelProperties(1, 1, parcelProperties(0, "Here"));
    REQUIRE(info.parcel());
    CHECK(info.parcel()->name == "Here");
    // A new region: its estate is asked for, once.
    auto r = info.requests();
    CHECK(r.estate);
    CHECK_FALSE(info.requests().estate);
    info.parcelProperties(1, 1, parcelProperties(5, "Next door"));
    CHECK(info.parcel()->name == "Next door");
    CHECK_FALSE(info.requests().estate);                      // same region
    info.parcelProperties(1, 1, parcelProperties(3, "Late")); // out of order
    CHECK(info.parcel()->name == "Next door");

    // Groups' names: asked once, then known.
    const Uuid group = info.parcel()->group;
    CHECK(info.group(group).empty());
    CHECK(info.group(group).empty());
    r = info.requests();
    REQUIRE(r.groups.size() == 1);
    CHECK(r.groups.front() == group);
    CHECK(info.requests().groups.empty());
    info.groupName(group, "Sandbox Group");
    CHECK(info.group(group) == "Sandbox Group");
    CHECK(info.group(Uuid()).empty()); // no group: not asked
    CHECK(info.requests().groups.empty());

    info.estate("Mainland", Uuid(), Uuid());
    REQUIRE(info.estate());
    CHECK(info.estate()->name == "Mainland");

    info.clear();
    CHECK_FALSE(info.parcel());
    CHECK_FALSE(info.estate());
}
