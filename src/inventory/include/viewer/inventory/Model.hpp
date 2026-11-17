#pragma once

// The agent's inventory as far as it's been fetched: folders and items by
// id, links included (an item pointing at another item, or at a folder).
// Filled from AISv3 replies (Ais), which carry what's inside a folder (or a
// link's target) in "_embedded" maps keyed by id. One per session (owned
// by the app, as the world is).

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer::inventory
{

// Asset types (llassettype.h) the viewer cares about by number.
enum AssetType : int
{
    kObject = 6,
    kLink = 24,
    kFolderLink = 25,
};

// Folder types (llfoldertype.h).
enum FolderType : int
{
    kCurrentOutfit = 46,
};

struct Permissions
{
    Uuid creator;
    Uuid owner;
    Uuid lastOwner;
    Uuid group;
    uint32_t baseMask = 0;
    uint32_t ownerMask = 0;
    uint32_t groupMask = 0;
    uint32_t everyoneMask = 0;
    uint32_t nextOwnerMask = 0;
    bool groupOwned = false;

    static Permissions fromLlsd(const llsd::Value& map);
};

struct Item
{
    Uuid id;
    Uuid parent;
    std::string name;
    std::string description;
    int type = -1;          // asset type
    int inventoryType = -1; // inventory type
    uint32_t flags = 0;
    Uuid assetId;  // what it is (a texture, an object, ...)
    Uuid linkedId; // a link's target (an item, or a folder for a folder link)
    Permissions permissions;
    int64_t createdAt = 0; // seconds since 1970 (UTC)

    bool isLink() const { return type == kLink || type == kFolderLink; }
    static std::optional<Item> fromLlsd(const llsd::Value& map);
};

struct Folder
{
    Uuid id;
    Uuid parent;
    std::string name;
    int type = -1;    // preferred type (-1: an ordinary folder)
    int version = -1; // bumped by the service on every change in it

    static std::optional<Folder> fromLlsd(const llsd::Value& map);
};

class Model
{
public:
    const Folder* folder(const Uuid& id) const;
    const Item* item(const Uuid& id) const;
    // A link's target item (or the item itself if it isn't a link); null if
    // it isn't known.
    const Item* resolve(const Uuid& id) const;
    // What a folder holds, as far as known.
    std::vector<const Item*> items(const Uuid& folder) const;
    std::vector<const Folder*> folders(const Uuid& parent) const;
    // A system folder by its type (the Current Outfit Folder, ...), if known.
    const Folder* folderOfType(int type) const;

    // What one merge brought (ids of the folders and items in it).
    struct Merged
    {
        std::vector<Uuid> folders;
        std::vector<Uuid> items;
    };
    // An AISv3 reply: a folder, an item or a link, with what's embedded in
    // it (folders, items, links, a link's target), however deep.
    Merged merge(const llsd::Value& reply);

    void put(Folder folder);
    void put(Item item);
    // An item (or link) gone.
    void remove(const Uuid& item);
    void clear();
    size_t folderCount() const { return folders_.size(); }
    size_t itemCount() const { return items_.size(); }

private:
    void mergeInto(const llsd::Value& node, Merged& merged);
    void mergeEmbedded(const llsd::Value& embedded, Merged& merged);

    std::unordered_map<Uuid, Folder> folders_;
    std::unordered_map<Uuid, Item> items_;
    // Children by parent folder.
    std::unordered_map<Uuid, std::unordered_set<Uuid>> childFolders_;
    std::unordered_map<Uuid, std::unordered_set<Uuid>> childItems_;
};

} // namespace viewer::inventory
