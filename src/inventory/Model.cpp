#include "viewer/inventory/Model.hpp"

namespace viewer::inventory
{

namespace
{

uint32_t mask(const llsd::Value& v)
{
    return static_cast<uint32_t>(v.asInteger());
}

} // namespace

Permissions Permissions::fromLlsd(const llsd::Value& p)
{
    Permissions out;
    out.creator = p["creator_id"].asUuid();
    out.owner = p["owner_id"].asUuid();
    out.lastOwner = p["last_owner_id"].asUuid();
    out.group = p["group_id"].asUuid();
    out.baseMask = mask(p["base_mask"]);
    out.ownerMask = mask(p["owner_mask"]);
    out.groupMask = mask(p["group_mask"]);
    out.everyoneMask = mask(p["everyone_mask"]);
    out.nextOwnerMask = mask(p["next_owner_mask"]);
    out.groupOwned = p["is_owner_group"].asBoolean();
    return out;
}

std::optional<Item> Item::fromLlsd(const llsd::Value& m)
{
    if(!m.isMap() || m["item_id"].isUndefined())
    {
        return std::nullopt;
    }
    Item item;
    item.id = m["item_id"].asUuid();
    item.parent = m["parent_id"].asUuid();
    item.name = m["name"].asString();
    item.description = m["desc"].asString();
    item.type = m["type"].asInteger();
    item.inventoryType = m["inv_type"].asInteger();
    item.flags = mask(m["flags"]);
    item.assetId = m["asset_id"].asUuid();
    item.linkedId = m["linked_id"].asUuid();
    if(item.isLink() && item.linkedId.isNull())
    {
        item.linkedId = item.assetId; // older replies give the target as the asset
    }
    item.permissions = Permissions::fromLlsd(m["permissions"]);
    return item;
}

std::optional<Folder> Folder::fromLlsd(const llsd::Value& m)
{
    if(!m.isMap() || m["category_id"].isUndefined())
    {
        return std::nullopt;
    }
    Folder f;
    f.id = m["category_id"].asUuid();
    f.parent = m["parent_id"].asUuid();
    f.name = m["name"].asString();
    f.type = m["type_default"].isUndefined() ? -1 : m["type_default"].asInteger();
    f.version = m["version"].isUndefined() ? -1 : m["version"].asInteger();
    return f;
}

const Folder* Model::folder(const Uuid& id) const
{
    const auto it = folders_.find(id);
    return it == folders_.end() ? nullptr : &it->second;
}

const Item* Model::item(const Uuid& id) const
{
    const auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

const Item* Model::resolve(const Uuid& id) const
{
    const Item* i = item(id);
    return i && i->type == kLink ? item(i->linkedId) : i;
}

std::vector<const Item*> Model::items(const Uuid& folder) const
{
    std::vector<const Item*> out;
    if(const auto it = childItems_.find(folder); it != childItems_.end())
    {
        for(const Uuid& id : it->second)
        {
            if(const Item* i = item(id))
            {
                out.push_back(i);
            }
        }
    }
    return out;
}

std::vector<const Folder*> Model::folders(const Uuid& parent) const
{
    std::vector<const Folder*> out;
    if(const auto it = childFolders_.find(parent); it != childFolders_.end())
    {
        for(const Uuid& id : it->second)
        {
            if(const Folder* f = folder(id))
            {
                out.push_back(f);
            }
        }
    }
    return out;
}

const Folder* Model::folderOfType(int type) const
{
    for(const auto& [id, f] : folders_)
    {
        if(f.type == type)
        {
            return &f;
        }
    }
    return nullptr;
}

void Model::put(Folder f)
{
    if(const auto old = folders_.find(f.id);
       old != folders_.end() && old->second.parent != f.parent)
    {
        childFolders_[old->second.parent].erase(f.id);
    }
    if(!f.parent.isNull())
    {
        childFolders_[f.parent].insert(f.id);
    }
    const Uuid id = f.id;
    folders_[id] = std::move(f);
}

void Model::put(Item i)
{
    if(const auto old = items_.find(i.id); old != items_.end() && old->second.parent != i.parent)
    {
        childItems_[old->second.parent].erase(i.id);
    }
    if(!i.parent.isNull())
    {
        childItems_[i.parent].insert(i.id);
    }
    const Uuid id = i.id;
    items_[id] = std::move(i);
}

void Model::clear()
{
    folders_.clear();
    items_.clear();
    childFolders_.clear();
    childItems_.clear();
}

Model::Merged Model::merge(const llsd::Value& reply)
{
    Merged merged;
    mergeInto(reply, merged);
    return merged;
}

void Model::mergeInto(const llsd::Value& node, Merged& merged)
{
    if(!node.isMap())
    {
        return;
    }
    if(auto f = Folder::fromLlsd(node))
    {
        merged.folders.push_back(f->id);
        put(std::move(*f));
    }
    else if(auto i = Item::fromLlsd(node))
    {
        merged.items.push_back(i->id);
        put(std::move(*i));
    }
    mergeEmbedded(node["_embedded"], merged);
}

void Model::mergeEmbedded(const llsd::Value& embedded, Merged& merged)
{
    if(!embedded.isMap())
    {
        return;
    }
    // Collections are maps keyed by id; a link's target is a single map.
    for(const char* collection : {"categories", "items", "links"})
    {
        const llsd::Value& c = embedded[collection];
        if(const llsd::Map* m = c.get<llsd::Map>())
        {
            for(const auto& [key, value] : *m)
            {
                mergeInto(value, merged);
            }
        }
        else if(c.isArray())
        {
            for(size_t i = 0; i < c.size(); i++)
            {
                mergeInto(c[i], merged);
            }
        }
    }
    for(const char* single : {"item", "category"})
    {
        mergeInto(embedded[single], merged);
    }
}

} // namespace viewer::inventory
