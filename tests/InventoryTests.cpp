#include "viewer/inventory/Model.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

using namespace viewer;
using namespace viewer::inventory;

namespace
{

Uuid id(const char* text)
{
    return *Uuid::parse(text);
}

// As AISv3 answers category/current/links: the links keyed by id, each
// with its target embedded; one target left out (to be fetched alone).
const char* kLinksReply = R"(<?xml version="1.0" ?><llsd><map>
<key>_embedded</key><map><key>links</key><map>
  <key>00000000-0000-0000-0000-0000000000b1</key><map>
    <key>item_id</key><uuid>00000000-0000-0000-0000-0000000000b1</uuid>
    <key>parent_id</key><uuid>00000000-0000-0000-0000-000000000046</uuid>
    <key>type</key><integer>24</integer>
    <key>linked_id</key><uuid>00000000-0000-0000-0000-000000000101</uuid>
    <key>_embedded</key><map><key>item</key><map>
      <key>item_id</key><uuid>00000000-0000-0000-0000-000000000101</uuid>
      <key>parent_id</key><uuid>00000000-0000-0000-0000-000000000077</uuid>
      <key>type</key><integer>6</integer><key>inv_type</key><integer>6</integer>
      <key>name</key><string>Hair</string><key>desc</key><string>skull</string>
      <key>flags</key><integer>2</integer>
      <key>permissions</key><map><key>owner_id</key><uuid>00000000-0000-0000-0000-0000000000cc</uuid>
        <key>next_owner_mask</key><integer>581632</integer></map>
    </map></map>
  </map>
  <key>00000000-0000-0000-0000-0000000000b2</key><map>
    <key>item_id</key><uuid>00000000-0000-0000-0000-0000000000b2</uuid>
    <key>parent_id</key><uuid>00000000-0000-0000-0000-000000000046</uuid>
    <key>type</key><integer>24</integer>
    <key>asset_id</key><uuid>00000000-0000-0000-0000-000000000102</uuid>
  </map>
</map></map></map></llsd>)";

} // namespace

TEST_CASE("Inventory: an AISv3 reply merged, links resolved to their embedded targets")
{
    Model model;
    const auto reply = llsd::parse(kLinksReply);
    REQUIRE(reply);
    const Model::Merged merged = model.merge(*reply);
    CHECK(merged.items.size() == 3); // two links, one target

    const Item* link = model.item(id("00000000-0000-0000-0000-0000000000b1"));
    REQUIRE(link);
    CHECK(link->isLink());
    const Item* hair = model.resolve(link->id);
    REQUIRE(hair);
    CHECK(hair->name == "Hair");
    CHECK(hair->type == kObject);
    CHECK(hair->permissions.owner == id("00000000-0000-0000-0000-0000000000cc"));
    CHECK(hair->permissions.nextOwnerMask == 581632);

    // A link given its target as the asset (older replies), not embedded:
    // known as a link, its target not yet.
    const Item* other = model.item(id("00000000-0000-0000-0000-0000000000b2"));
    REQUIRE(other);
    CHECK(other->linkedId == id("00000000-0000-0000-0000-000000000102"));
    CHECK_FALSE(model.resolve(other->id));

    // Children by folder.
    CHECK(model.items(id("00000000-0000-0000-0000-000000000046")).size() == 2);
    CHECK(model.items(id("00000000-0000-0000-0000-000000000077")).size() == 1);

    // A folder, its type kept.
    const auto folder = llsd::parse(R"(<?xml version="1.0" ?><llsd><map>
      <key>category_id</key><uuid>00000000-0000-0000-0000-000000000046</uuid>
      <key>parent_id</key><uuid>00000000-0000-0000-0000-0000000000aa</uuid>
      <key>name</key><string>Current Outfit</string>
      <key>type_default</key><integer>46</integer><key>version</key><integer>12</integer>
    </map></llsd>)");
    REQUIRE(folder);
    model.merge(*folder);
    const Folder* cof = model.folderOfType(kCurrentOutfit);
    REQUIRE(cof);
    CHECK(cof->version == 12);
    CHECK(model.folders(id("00000000-0000-0000-0000-0000000000aa")).size() == 1);
}
