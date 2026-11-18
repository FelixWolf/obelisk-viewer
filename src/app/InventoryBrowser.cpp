#include "InventoryBrowser.hpp"

#include <algorithm>
#include <cctype>

namespace viewer
{

namespace
{

bool lessByName(std::string_view a, std::string_view b)
{
    return std::ranges::lexicographical_compare(a, b, [](unsigned char x, unsigned char y)
                                                { return std::tolower(x) < std::tolower(y); });
}

} // namespace

InventoryKind inventoryKind(int assetType, int inventoryType)
{
    switch(assetType)
    {
    case 0:
        return inventoryType == 15 ? InventoryKind{"Snapshot", "Pic", "texture"}
                                   : InventoryKind{"Texture", "Tex", "texture"};
    case 1:
        return {"Sound", "Snd", "sound"};
    case 2:
        return {"Calling card", "Card", "card"};
    case 3:
        return {"Landmark", "LM", "landmark"};
    case 5:
        return {"Clothing", "Clo", "wearable"};
    case 6:
        return {"Object", "Obj", "object"};
    case 7:
        return {"Notecard", "Note", "notecard"};
    case 10:
        return {"Script", "Scr", "script"};
    case 13:
        return {"Body part", "Body", "wearable"};
    case 20:
        return {"Animation", "Anim", "animation"};
    case 21:
        return {"Gesture", "Gest", "animation"};
    case 49:
        return {"Mesh", "Mesh", "object"};
    case 56:
        return {"Settings", "Env", "settings"};
    case 57:
        return {"Material", "Mat", "texture"};
    default:
        return {"Item", "?", "other"};
    }
}

std::vector<InventoryEntry> inventoryEntries(const inventory::Model& model, const Uuid& folder)
{
    std::vector<InventoryEntry> folders;
    std::vector<InventoryEntry> items;
    for(const inventory::Folder* f : model.folders(folder))
    {
        folders.push_back({f->id, true, f->name, {"Folder", "", "folder"}, false, false});
    }
    for(const inventory::Item* i : model.items(folder))
    {
        InventoryEntry e{i->id, false, i->name, {}, i->isLink(), false};
        if(i->type == inventory::kFolderLink)
        {
            e.folder = true;
            e.kind = {"Folder", "", "folder"};
            e.broken = !model.folder(i->linkedId);
            folders.push_back(std::move(e));
            continue;
        }
        const inventory::Item* target = model.resolve(i->id);
        e.broken = !target;
        e.kind = target ? inventoryKind(target->type, target->inventoryType)
                        : InventoryKind{"Link", "?", "other"};
        items.push_back(std::move(e));
    }
    const auto byName = [](const InventoryEntry& a, const InventoryEntry& b)
    { return lessByName(a.name, b.name); };
    std::ranges::sort(folders, byName);
    std::ranges::sort(items, byName);
    folders.insert(folders.end(), std::make_move_iterator(items.begin()),
                   std::make_move_iterator(items.end()));
    return folders;
}

std::optional<Uuid> InventoryBrowser::root()
{
    if(!root_ && !rootAsked_ && ais_.available())
    {
        rootAsked_ = true;
        ais_.fetchChildren(
            "root", 1,
            [this, generation = generation_](bool ok, const inventory::Model::Merged& merged)
            {
                if(generation != generation_)
                {
                    return;
                }
                if(!ok || merged.folders.empty())
                {
                    rootAsked_ = false; // asked again when wanted
                    return;
                }
                // The folder asked for comes first.
                root_ = merged.folders.front();
                folders_[*root_] = State::Loaded;
                version_++;
            });
    }
    return root_;
}

InventoryBrowser::State InventoryBrowser::open(const Uuid& folder)
{
    const auto known = folders_.find(folder);
    if(known != folders_.end())
    {
        return known->second;
    }
    fetch(folder);
    return State::Loading;
}

InventoryBrowser::State InventoryBrowser::openLinks(const Uuid& folder)
{
    const auto known = links_.find(folder);
    if(known != links_.end())
    {
        return known->second;
    }
    links_[folder] = State::Loading;
    version_++;
    ais_.fetchLinks(
        folder.toString(),
        [this, folder, generation = generation_](bool ok, const inventory::Model::Merged&)
        {
            if(generation != generation_)
            {
                return;
            }
            links_[folder] = ok ? State::Loaded : State::Failed;
            version_++;
        });
    return State::Loading;
}

void InventoryBrowser::refresh(const Uuid& folder)
{
    if(folders_[folder] != State::Loading)
    {
        fetch(folder);
    }
}

void InventoryBrowser::fetch(const Uuid& folder)
{
    folders_[folder] = State::Loading;
    version_++;
    ais_.fetchChildren(
        folder.toString(), 1,
        [this, folder, generation = generation_](bool ok, const inventory::Model::Merged&)
        {
            if(generation != generation_)
            {
                return;
            }
            folders_[folder] = ok ? State::Loaded : State::Failed;
            version_++;
        });
}

void InventoryBrowser::clear()
{
    root_.reset();
    rootAsked_ = false;
    folders_.clear();
    links_.clear();
    generation_++;
    version_++;
}

} // namespace viewer
