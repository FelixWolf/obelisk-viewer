#include "OutfitAttachments.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>

namespace viewer
{

namespace
{

// A map's value under either of two keys (the inventory services name
// some fields differently).
const llsd::Value& either(const llsd::Value& map, const char* a, const char* b)
{
    const llsd::Value& first = map[a];
    return first.isUndefined() ? map[b] : first;
}

} // namespace

OutfitAttachments::OutfitAttachments(net::HttpClient& http)
    : http_(http), requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void OutfitAttachments::clear()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    replies_ = std::make_shared<MessageQueue<net::HttpResponse>>();
    state_ = State::Idle;
    ais_ = nullptr;
    model_ = nullptr;
    links_.clear();
    targetsAwaited_ = 0;
    outfit_.clear();
    pending_.clear();
    passes_ = 0;
    nextPass_.reset();
    wornCount_ = 0;
    wornChanged_ = 0.0;
}

void OutfitAttachments::post(const std::string& url, const llsd::Value& body)
{
    net::HttpRequest request{.method = net::HttpMethod::Post, .url = url};
    request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    const std::string xml = llsd::toXml(body);
    request.body.assign(xml.begin(), xml.end());
    requests_->send(std::move(request), [replies = replies_](net::HttpResponse&& response)
                    { replies->push(std::move(response)); });
}

void OutfitAttachments::fetchFolder(const Uuid& folder, bool folders, bool items)
{
    llsd::Value f{llsd::Map{}};
    f["folder_id"] = folder;
    f["owner_id"] = agent_;
    f["fetch_folders"] = folders;
    f["fetch_items"] = items;
    f["sort_order"] = 0;
    llsd::Value body{llsd::Map{}};
    body["folders"] = llsd::Value(llsd::Array{});
    body["folders"].append(std::move(f));
    post(fetchDescendents_, body);
}

void OutfitAttachments::start(inventory::Ais& ais, const inventory::Model& model,
                              std::string fetchDescendents, std::string fetchItems,
                              const Uuid& agent, const Uuid& inventoryRoot)
{
    if(state_ != State::Idle)
    {
        return;
    }
    if(ais.available())
    {
        ais_ = &ais;
        model_ = &model;
        agent_ = agent;
        state_ = State::FetchingOutfit;
        ais.fetchLinks("current", [this](bool ok, const inventory::Model::Merged& merged)
                       { linksArrived(ok, merged); });
        return;
    }
    if(fetchDescendents.empty() || fetchItems.empty() || inventoryRoot.isNull())
    {
        log::warn("outfit: no inventory capabilities or root; attachments as the region has them");
        state_ = State::Failed;
        return;
    }
    fetchDescendents_ = std::move(fetchDescendents);
    fetchItems_ = std::move(fetchItems);
    agent_ = agent;
    state_ = State::FindingFolder;
    fetchFolder(inventoryRoot, true, false);
}

std::optional<Uuid> OutfitAttachments::outfitFolder(const llsd::Value& reply)
{
    const llsd::Value& folders = reply["folders"];
    for(size_t f = 0; f < folders.size(); f++)
    {
        const llsd::Value& categories = folders[f]["categories"];
        for(size_t c = 0; c < categories.size(); c++)
        {
            const llsd::Value& category = categories[c];
            if(either(category, "type_default", "preferred_type").asInteger() == kOutfitFolderType)
            {
                return either(category, "category_id", "folder_id").asUuid();
            }
        }
    }
    return std::nullopt;
}

std::vector<Uuid> OutfitAttachments::linkedItems(const llsd::Value& reply)
{
    std::vector<Uuid> linked;
    const llsd::Value& folders = reply["folders"];
    for(size_t f = 0; f < folders.size(); f++)
    {
        const llsd::Value& items = folders[f]["items"];
        for(size_t i = 0; i < items.size(); i++)
        {
            const llsd::Value& item = items[i];
            if(item["type"].asInteger() != kLinkType)
            {
                continue;
            }
            const Uuid target = either(item, "linked_id", "asset_id").asUuid();
            if(!target.isNull() && std::ranges::find(linked, target) == linked.end())
            {
                linked.push_back(target);
            }
        }
    }
    return linked;
}

std::vector<OutfitAttachments::Item> OutfitAttachments::objects(const llsd::Value& reply)
{
    std::vector<Item> out;
    const llsd::Value& items = reply["items"];
    for(size_t i = 0; i < items.size(); i++)
    {
        const llsd::Value& item = items[i];
        if(item["type"].asInteger() != kObjectType)
        {
            continue;
        }
        const llsd::Value& p = item["permissions"];
        Item o;
        o.itemId = item["item_id"].asUuid();
        o.ownerId = p["owner_id"].asUuid();
        o.flags = static_cast<uint32_t>(item["flags"].asInteger());
        o.groupMask = static_cast<uint32_t>(p["group_mask"].asInteger());
        o.everyoneMask = static_cast<uint32_t>(p["everyone_mask"].asInteger());
        o.nextOwnerMask = static_cast<uint32_t>(p["next_owner_mask"].asInteger());
        o.name = item["name"].asString();
        o.description = item["desc"].asString();
        if(!o.itemId.isNull())
        {
            out.push_back(std::move(o));
        }
    }
    return out;
}

OutfitAttachments::Item OutfitAttachments::toAttach(const inventory::Item& item)
{
    Item o;
    o.itemId = item.id;
    o.ownerId = item.permissions.owner;
    o.flags = item.flags;
    o.groupMask = item.permissions.groupMask;
    o.everyoneMask = item.permissions.everyoneMask;
    o.nextOwnerMask = item.permissions.nextOwnerMask;
    o.name = item.name;
    o.description = item.description;
    return o;
}

void OutfitAttachments::linksArrived(bool ok, const inventory::Model::Merged& merged)
{
    if(!ok)
    {
        log::warn("outfit: the Current Outfit Folder couldn't be fetched; attachments as the "
                  "region has them");
        state_ = State::Failed;
        return;
    }
    // The links (their targets came embedded, mostly).
    for(const Uuid& id : merged.items)
    {
        const inventory::Item* item = model_->item(id);
        if(item && item->type == inventory::kLink)
        {
            links_.push_back(id);
        }
    }
    // Targets not embedded: fetched on their own.
    std::vector<Uuid> missing;
    for(const Uuid& link : links_)
    {
        const inventory::Item* item = model_->item(link);
        if(item && !model_->item(item->linkedId))
        {
            missing.push_back(item->linkedId);
        }
    }
    targetsAwaited_ = static_cast<int>(missing.size());
    for(const Uuid& id : missing)
    {
        ais_->fetchItem(id, [this](bool fetched, const inventory::Model::Merged&)
                        { targetArrived(fetched); });
    }
    if(targetsAwaited_ == 0)
    {
        finishFromModel();
    }
}

void OutfitAttachments::targetArrived(bool)
{
    if(--targetsAwaited_ == 0)
    {
        finishFromModel();
    }
}

void OutfitAttachments::finishFromModel()
{
    for(const Uuid& link : links_)
    {
        const inventory::Item* target = model_->resolve(link);
        if(target && target->type == inventory::kObject &&
           std::ranges::find(outfit_, target->id, &Item::itemId) == outfit_.end())
        {
            outfit_.push_back(toAttach(*target));
        }
    }
    log::info("outfit: {} objects in the Current Outfit Folder (AISv3)", outfit_.size());
    state_ = State::Ready;
}

std::vector<OutfitAttachments::Item> OutfitAttachments::update(double now,
                                                               std::optional<double> arrived,
                                                               const std::unordered_set<Uuid>& worn)
{
    replies_->drain(drained_);
    for(const net::HttpResponse& response : drained_)
    {
        if(state_ == State::Ready || state_ == State::Failed || state_ == State::Idle)
        {
            break;
        }
        std::optional<llsd::Value> reply;
        if(response.ok())
        {
            if(auto parsed = llsd::parse(response.bodyText()))
            {
                reply = std::move(*parsed);
            }
        }
        if(!reply)
        {
            log::warn("outfit: an inventory fetch failed (HTTP {}); attachments as the region "
                      "has them",
                      response.status);
            state_ = State::Failed;
            break;
        }
        if(state_ == State::FindingFolder)
        {
            const auto folder = outfitFolder(*reply);
            if(!folder)
            {
                log::warn("outfit: no Current Outfit Folder");
                state_ = State::Failed;
                break;
            }
            state_ = State::FetchingOutfit;
            fetchFolder(*folder, false, true);
        }
        else if(state_ == State::FetchingOutfit)
        {
            const std::vector<Uuid> linked = linkedItems(*reply);
            if(linked.empty())
            {
                state_ = State::Ready;
                break;
            }
            llsd::Value body{llsd::Map{}};
            body["agent_id"] = agent_;
            body["items"] = llsd::Value(llsd::Array{});
            for(const Uuid& id : linked)
            {
                llsd::Value i{llsd::Map{}};
                i["owner_id"] = agent_;
                i["item_id"] = id;
                body["items"].append(std::move(i));
            }
            state_ = State::FetchingItems;
            post(fetchItems_, body);
        }
        else if(state_ == State::FetchingItems)
        {
            outfit_ = objects(*reply);
            log::info("outfit: {} objects in the Current Outfit Folder", outfit_.size());
            state_ = State::Ready;
        }
    }

    std::vector<Item> now_;
    if(state_ != State::Ready || !arrived)
    {
        return now_;
    }
    // Settled: in a moment, and no attachments arriving for a while (the
    // simulator's own restores done).
    if(worn.size() != wornCount_)
    {
        wornCount_ = worn.size();
        wornChanged_ = now;
    }
    const bool settled = *arrived >= kSettle && now - wornChanged_ >= kQuiet;
    // A pass: what isn't worn, once settled (then once more, later).
    if(passes_ < 2 && pending_.empty() && settled && (!nextPass_ || now >= *nextPass_))
    {
        for(const Item& item : outfit_)
        {
            if(!worn.contains(item.itemId))
            {
                pending_.push_back(item);
            }
        }
        if(!pending_.empty())
        {
            log::info("outfit: wearing {} attachments the region didn't bring back{}",
                      pending_.size(), passes_ > 0 ? " (again)" : "");
        }
        passes_++;
        nextPass_ = now + kRetry;
    }
    // Five a frame (at most ten a second); any worn meanwhile left out.
    std::erase_if(pending_, [&](const Item& item) { return worn.contains(item.itemId); });
    if(!pending_.empty() && now - lastSent_ >= 0.1)
    {
        const size_t count = std::min(pending_.size(), kPerRequest);
        now_.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
        lastSent_ = now;
    }
    return now_;
}

} // namespace viewer
