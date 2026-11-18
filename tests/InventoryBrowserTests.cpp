#include "InventoryBrowser.hpp"

#include <doctest/doctest.h>

#include <format>

using namespace viewer;

namespace
{

Uuid id(int n)
{
    return *Uuid::parse(std::format("00000000-0000-0000-0000-{:012}", n));
}

} // namespace

TEST_CASE("Inventory browser: a folder's lines, folders first, each by name")
{
    inventory::Model model;
    const Uuid root = id(1);
    model.put(inventory::Folder{root, Uuid(), "My Inventory", 8, 1, {}});
    model.put(inventory::Folder{id(2), root, "textures", -1, 1, {}});
    model.put(inventory::Folder{id(3), root, "Clothing", 5, 1, {}});
    model.put(inventory::Folder{id(4), Uuid(), "Elsewhere", -1, 1, {}});

    inventory::Item texture;
    texture.id = id(10);
    texture.parent = root;
    texture.name = "zebra";
    texture.type = 0;
    texture.inventoryType = 0;
    model.put(texture);
    inventory::Item snapshot = texture;
    snapshot.id = id(11);
    snapshot.name = "Beach";
    snapshot.inventoryType = 15;
    model.put(snapshot);
    inventory::Item link;
    link.id = id(12);
    link.parent = root;
    link.name = "apple (link)";
    link.type = inventory::kLink;
    link.linkedId = id(10);
    model.put(link);
    inventory::Item broken = link;
    broken.id = id(13);
    broken.name = "Lost";
    broken.linkedId = id(99);
    model.put(broken);
    inventory::Item folderLink;
    folderLink.id = id(14);
    folderLink.parent = root;
    folderLink.name = "An outfit";
    folderLink.type = inventory::kFolderLink;
    folderLink.linkedId = id(4);
    model.put(folderLink);

    const auto lines = inventoryEntries(model, root);
    REQUIRE(lines.size() == 7);
    // Folders (a folder link among them), by name ignoring case.
    CHECK(lines[0].name == "An outfit");
    CHECK(lines[0].folder);
    CHECK(lines[0].link);
    CHECK_FALSE(lines[0].broken);
    CHECK(lines[1].name == "Clothing");
    CHECK(lines[2].name == "textures");
    // Then items: links as what they point at.
    CHECK(lines[3].name == "apple (link)");
    CHECK(lines[3].link);
    CHECK(lines[3].kind.name == "Texture");
    CHECK(lines[4].name == "Beach");
    CHECK(lines[4].kind.name == "Snapshot");
    CHECK(lines[5].name == "Lost");
    CHECK(lines[5].broken);
    CHECK(lines[6].name == "zebra");
    CHECK(lines[6].kind.style == "texture");

    CHECK(inventoryKind(6, 6).name == "Object");
    CHECK(inventoryKind(3, 3).name == "Landmark");
    CHECK(inventoryKind(999, 0).name == "Item");
}
