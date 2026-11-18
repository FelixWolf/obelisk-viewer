#pragma once

// Browsing the inventory (the main menu's Inventory tab), as a file
// explorer does: the root folder (AISv3's "root" alias), and a folder's
// contents, asked for the first time it's opened (then kept as the model
// has them). What a folder shows: its folders, then its items, each by
// name; links as what they point at.

#include "viewer/core/Uuid.hpp"
#include "viewer/inventory/Ais.hpp"
#include "viewer/inventory/Model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace viewer
{

// What kind of thing an item is, for its label and badge.
struct InventoryKind
{
    std::string_view name;  // "Texture", "Object", ...
    std::string_view badge; // a few letters
    std::string_view style; // for the page's style: "texture", "object", ...
};
// By asset type (llassettype.h), and inventory type for those that share
// one (a snapshot is a texture).
InventoryKind inventoryKind(int assetType, int inventoryType);

// One line of a folder's contents.
struct InventoryEntry
{
    Uuid id; // the folder's, or the item's (a link's own)
    bool folder = false;
    std::string name;
    InventoryKind kind;
    bool link = false;   // a link (to an item or a folder)
    bool broken = false; // a link whose target isn't known
};
// A folder's contents as known: folders, then items, each by name
// (ignoring case); folder links are listed as folders.
std::vector<InventoryEntry> inventoryEntries(const inventory::Model& model, const Uuid& folder);

class InventoryBrowser
{
public:
    InventoryBrowser(inventory::Ais& ais, inventory::Model& model) : ais_(ais), model_(model) {}

    // The root folder; nothing until it's here (it's asked for).
    std::optional<Uuid> root();

    enum class State
    {
        Loading,
        Loaded,
        Failed,
    };
    // A folder's contents: asked for the first time.
    State open(const Uuid& folder);
    // A folder's links, each with what it points at (an outfit's): asked
    // for the first time.
    State openLinks(const Uuid& folder);
    // Asks for a folder's contents again.
    void refresh(const Uuid& folder);

    const inventory::Model& model() const { return model_; }
    // Changes when anything shown might have.
    uint64_t version() const { return version_; }
    void clear();

private:
    void fetch(const Uuid& folder);

    inventory::Ais& ais_;
    inventory::Model& model_;
    std::optional<Uuid> root_;
    bool rootAsked_ = false;
    std::unordered_map<Uuid, State> folders_;
    std::unordered_map<Uuid, State> links_;
    uint64_t generation_ = 0; // of answers wanted (clear() forgets those coming)
    uint64_t version_ = 0;
};

} // namespace viewer
