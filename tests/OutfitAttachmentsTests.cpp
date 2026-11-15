#include "OutfitAttachments.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

using namespace viewer;

namespace
{

llsd::Value xml(const std::string& text)
{
    auto parsed = llsd::parse(text);
    REQUIRE(parsed);
    return *parsed;
}

const char* kRootReply = R"(<?xml version="1.0" ?><llsd><map><key>folders</key><array><map>
<key>folder_id</key><uuid>00000000-0000-0000-0000-0000000000aa</uuid>
<key>categories</key><array>
  <map><key>category_id</key><uuid>00000000-0000-0000-0000-000000000001</uuid>
       <key>name</key><string>Objects</string><key>type_default</key><integer>6</integer></map>
  <map><key>category_id</key><uuid>00000000-0000-0000-0000-000000000046</uuid>
       <key>name</key><string>Current Outfit</string><key>type_default</key><integer>46</integer></map>
</array></map></array></map></llsd>)";

const char* kOutfitReply = R"(<?xml version="1.0" ?><llsd><map><key>folders</key><array><map>
<key>items</key><array>
  <map><key>item_id</key><uuid>00000000-0000-0000-0000-0000000000b1</uuid><key>type</key><integer>24</integer>
       <key>asset_id</key><uuid>00000000-0000-0000-0000-000000000101</uuid></map>
  <map><key>item_id</key><uuid>00000000-0000-0000-0000-0000000000b2</uuid><key>type</key><integer>24</integer>
       <key>linked_id</key><uuid>00000000-0000-0000-0000-000000000102</uuid></map>
  <map><key>item_id</key><uuid>00000000-0000-0000-0000-0000000000b3</uuid><key>type</key><integer>25</integer>
       <key>asset_id</key><uuid>00000000-0000-0000-0000-000000000103</uuid></map>
</array></map></array></map></llsd>)";

const char* kItemsReply = R"(<?xml version="1.0" ?><llsd><map><key>items</key><array>
  <map><key>item_id</key><uuid>00000000-0000-0000-0000-000000000101</uuid><key>type</key><integer>6</integer>
       <key>name</key><string>Head</string><key>desc</key><string>skull</string><key>flags</key><integer>2</integer>
       <key>permissions</key><map><key>owner_id</key><uuid>00000000-0000-0000-0000-0000000000cc</uuid>
         <key>group_mask</key><integer>0</integer><key>everyone_mask</key><integer>0</integer>
         <key>next_owner_mask</key><integer>581632</integer></map></map>
  <map><key>item_id</key><uuid>00000000-0000-0000-0000-000000000102</uuid><key>type</key><integer>13</integer>
       <key>name</key><string>Shape</string></map>
</array></map></llsd>)";

Uuid id(const char* text)
{
    return *Uuid::parse(text);
}

} // namespace

TEST_CASE("Outfit: the Current Outfit Folder found, its links followed, its objects kept")
{
    CHECK(OutfitAttachments::outfitFolder(xml(kRootReply)) ==
          id("00000000-0000-0000-0000-000000000046"));
    // Links only (a folder link left out), by linked_id or asset_id.
    const auto linked = OutfitAttachments::linkedItems(xml(kOutfitReply));
    REQUIRE(linked.size() == 2);
    CHECK(linked[0] == id("00000000-0000-0000-0000-000000000101"));
    CHECK(linked[1] == id("00000000-0000-0000-0000-000000000102"));
    // Objects only (the shape left out), with what attaching needs.
    const auto objects = OutfitAttachments::objects(xml(kItemsReply));
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].itemId == id("00000000-0000-0000-0000-000000000101"));
    CHECK(objects[0].ownerId == id("00000000-0000-0000-0000-0000000000cc"));
    CHECK(objects[0].name == "Head");
    CHECK(objects[0].description == "skull");
    CHECK(objects[0].flags == 2);
    CHECK(objects[0].nextOwnerMask == 581632);
}
