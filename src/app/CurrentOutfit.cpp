#include "CurrentOutfit.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>

namespace viewer
{

CurrentOutfit::CurrentOutfit(net::HttpClient& http, inventory::Ais& ais, inventory::Model& model)
    : http_(http), ais_(ais), model_(model),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

CurrentOutfit::~CurrentOutfit() = default;

void CurrentOutfit::attaching(const Uuid& item, double now)
{
    detaching_.erase(item);
    attaching_[item] = now;
}

void CurrentOutfit::detaching(const Uuid& item, double now)
{
    attaching_.erase(item);
    detaching_[item] = now;
}

void CurrentOutfit::wear(const Uuid& item, How how)
{
    changes_.push_back({item, true, how});
    changing_.insert(item);
}

void CurrentOutfit::takeOff(const Uuid& item)
{
    changes_.push_back({item, false, How::Replace});
    changing_.insert(item);
}

void CurrentOutfit::wearOutfit(const Uuid& folder)
{
    outfit_ = folder;
}

Uuid CurrentOutfit::baseOutfit()
{
    const inventory::Folder* cof = outfitFolder();
    if(!cof)
    {
        return {};
    }
    for(const inventory::Item* i : model_.items(cof->id))
    {
        if(i->type == inventory::kFolderLink)
        {
            return i->linkedId;
        }
    }
    return {};
}

CurrentOutfit::AttachmentChanges
CurrentOutfit::takeAttachmentChanges(const std::unordered_set<Uuid>& worn)
{
    AttachmentChanges changes;
    if(!syncAttachments_ || writes_ != 0)
    {
        return changes;
    }
    syncAttachments_ = false;
    for(const Uuid& item : outfitObjects_)
    {
        if(!worn.contains(item))
        {
            changes.attach.push_back(item);
        }
    }
    for(const Uuid& item : worn)
    {
        if(!outfitObjects_.contains(item))
        {
            changes.detach.push_back(item);
        }
    }
    return changes;
}

void CurrentOutfit::slamOutfit(const inventory::Folder& cof, const Uuid& outfit)
{
    // The outfit's items (its links' targets, or items in it), each with
    // its own description (a link's orders layered clothing).
    struct Entry
    {
        const inventory::Item* item;
        std::string description;
    };
    std::vector<Entry> bodyParts;
    std::vector<Entry> clothing;
    std::vector<Entry> objects;
    std::vector<Entry> gestures;
    const auto sort = [&](const Uuid& folder, bool outfitOnly)
    {
        for(const inventory::Item* i : model_.items(folder))
        {
            const inventory::Item* target = model_.resolve(i->id);
            if(!target || i->type == inventory::kFolderLink)
            {
                continue;
            }
            const Entry e{target, i->description};
            switch(target->type)
            {
            case kBodyPart:
                bodyParts.push_back(e);
                break;
            case kClothing:
                if(outfitOnly)
                {
                    clothing.push_back(e);
                }
                break;
            case inventory::kObject:
                if(outfitOnly)
                {
                    objects.push_back(e);
                }
                break;
            case inventory::kGesture:
                if(outfitOnly)
                {
                    gestures.push_back(e);
                }
                break;
            default:
                break;
            }
        }
    };
    sort(outfit, true);
    // Nothing known in it (its contents not fetched?): not worn, rather
    // than everything taken off.
    if(bodyParts.empty() && clothing.empty() && objects.empty() && gestures.empty())
    {
        log::warn("outfit: nothing to wear in that outfit");
        return;
    }
    sort(cof.id, false); // body parts worn, after the outfit's
    // One body part of each kind: the outfit's first.
    std::unordered_set<int> kinds;
    std::erase_if(bodyParts,
                  [&](const Entry& e) { return !kinds.insert(wearableType(*e.item)).second; });
    std::vector<inventory::Ais::Link> links;
    std::unordered_set<Uuid> linked;
    outfitObjects_.clear();
    for(const auto* list : {&bodyParts, &clothing, &objects, &gestures})
    {
        for(const Entry& e : *list)
        {
            if(linked.insert(e.item->id).second)
            {
                links.push_back({e.item->id, inventory::kLink, e.item->inventoryType, e.item->name,
                                 e.description});
                if(e.item->type == inventory::kObject)
                {
                    outfitObjects_.insert(e.item->id);
                }
            }
        }
    }
    // And the outfit itself (the outfit worn).
    if(const inventory::Folder* f = model_.folder(outfit); f && f->type == inventory::kOutfit)
    {
        links.push_back({f->id, inventory::kFolderLink, -1, f->name, ""});
    }
    log::info("outfit: wearing an outfit: {} body parts, {} clothing, {} attachments, {} gestures",
              bodyParts.size(), clothing.size(), objects.size(), gestures.size());
    writes_++;
    ais_.slamLinks(cof.id, links,
                   [this, generation = generation_](bool ok, const inventory::Model::Merged&)
                   {
                       if(generation != generation_)
                       {
                           return;
                       }
                       writes_--;
                       if(!ok)
                       {
                           log::warn("outfit: the Current Outfit Folder couldn't be replaced");
                           syncAttachments_ = false;
                       }
                   });
    syncAttachments_ = true;
    lookWanted_ = true;
}

bool CurrentOutfit::pending(const Uuid& item) const
{
    return attaching_.contains(item) || detaching_.contains(item) || changing_.contains(item);
}

bool CurrentOutfit::wearing(const Uuid& item)
{
    const inventory::Folder* cof = outfitFolder();
    return cof && !linksTo(*cof, item).empty();
}

const inventory::Folder* CurrentOutfit::outfitFolder()
{
    if(fetched_)
    {
        return model_.folderOfType(inventory::kCurrentOutfit);
    }
    if(!fetching_ && ais_.available())
    {
        fetching_ = true;
        ais_.fetchChildren(
            "current", 1,
            [this, generation = generation_](bool ok, const inventory::Model::Merged&)
            {
                if(generation != generation_)
                {
                    return;
                }
                fetching_ = false;
                fetched_ = ok;
                if(!ok)
                {
                    log::warn("outfit: the Current Outfit Folder couldn't be had");
                }
            });
    }
    return nullptr;
}

std::vector<Uuid> CurrentOutfit::linksTo(const inventory::Folder& cof, const Uuid& item) const
{
    std::vector<Uuid> found;
    for(const inventory::Item* i : model_.items(cof.id))
    {
        if(i->isLink() && i->linkedId == item)
        {
            found.push_back(i->id);
        }
    }
    return found;
}

void CurrentOutfit::changeWearables(const inventory::Folder& cof)
{
    std::vector<inventory::Ais::Link> made;
    std::unordered_set<Uuid> removed;
    for(const Change& change : changes_)
    {
        const inventory::Item* item = model_.item(change.item);
        if(!item || !isWearable(*item))
        {
            changing_.erase(change.item);
            continue;
        }
        if(!change.on)
        {
            if(item->type == kBodyPart)
            {
                changing_.erase(change.item); // one of each is always worn
                continue;
            }
            for(const Uuid& link : linksTo(cof, change.item))
            {
                removed.insert(link);
            }
            continue;
        }
        // Instead of others of its kind (a body part always).
        if(item->type == kBodyPart || change.how == How::Replace)
        {
            for(const inventory::Item* link : model_.items(cof.id))
            {
                const inventory::Item* other =
                    link->isLink() ? model_.item(link->linkedId) : nullptr;
                if(other && other->id != item->id && other->type == item->type &&
                   wearableType(*other) == wearableType(*item))
                {
                    removed.insert(link->id);
                }
            }
        }
        const bool linked = !linksTo(cof, change.item).empty() ||
                            std::ranges::any_of(made, [&](const inventory::Ais::Link& l)
                                                { return l.target == change.item; });
        if(!linked)
        {
            made.push_back({item->id, inventory::kLink, item->inventoryType, item->name, ""});
        }
    }
    changes_.clear();

    const auto written = [this, generation = generation_](bool ok, const inventory::Model::Merged&)
    {
        if(generation != generation_)
        {
            return;
        }
        writes_--;
        if(!ok)
        {
            log::warn("outfit: a change to the Current Outfit Folder failed");
        }
    };
    if(!made.empty())
    {
        writes_++;
        ais_.createLinks(cof.id, made, written);
    }
    for(const Uuid& link : removed)
    {
        writes_++;
        ais_.removeItem(link, written);
    }
    log::info("outfit: wearing {} and taking off {} in the Current Outfit Folder", made.size(),
              removed.size());
    lookWanted_ = true;
}

void CurrentOutfit::askForLook(int version)
{
    if(appearanceUrl_.empty())
    {
        look_ = Look::Idle;
        changing_.clear();
        log::warn("outfit: the region can't make our look (no UpdateAvatarAppearance)");
        return;
    }
    look_ = Look::Asking;
    log::info("outfit: asking for our look from the Current Outfit Folder (version {})", version);
    llsd::Value body;
    body["cof_version"] = version;
    net::HttpRequest request{.method = net::HttpMethod::Post, .url = appearanceUrl_};
    request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    const std::string xml = llsd::toXml(body);
    request.body.assign(xml.begin(), xml.end());
    requests_->send(std::move(request),
                    [answers = answers_, generation = generation_](net::HttpResponse&& response)
                    { answers->push({generation, std::move(response)}); });
}

void CurrentOutfit::update(const std::unordered_set<Uuid>& worn, double now)
{
    // Our look's answer.
    std::vector<LookAnswer> answers;
    answers_->drain(answers);
    for(LookAnswer& a : answers)
    {
        if(a.generation != generation_ || look_ != Look::Asking)
        {
            continue;
        }
        const auto body = llsd::parse(a.response.bodyText());
        const bool success = a.response.ok() && body && (*body)["success"].asBoolean();
        if(success)
        {
            look_ = Look::Idle;
            retries_ = 0;
            changing_.clear();
            continue;
        }
        // A stale version: again with the one expected.
        if(body && (*body).has("expected") && retries_ < kRetries)
        {
            retries_++;
            askForLook((*body)["expected"].asInteger());
            continue;
        }
        log::warn("outfit: our look couldn't be made (HTTP {}{}{})", a.response.status,
                  body && (*body).has("error") ? ": " : "",
                  body ? (*body)["error"].asString() : std::string());
        look_ = Look::Idle;
        retries_ = 0;
        changing_.clear();
    }

    if(attaching_.empty() && detaching_.empty() && changes_.empty() && !lookWanted_ && !outfit_)
    {
        return;
    }
    const inventory::Folder* cof = outfitFolder();
    if(!cof)
    {
        return; // once it's known
    }

    // Wearables: the COF changed, then our look asked for (the COF's version
    // after the changes, fetched again).
    if(outfit_ && look_ == Look::Idle && writes_ == 0)
    {
        slamOutfit(*cof, *outfit_);
        outfit_.reset();
    }
    if(!changes_.empty() && look_ == Look::Idle && writes_ == 0)
    {
        changeWearables(*cof);
    }
    if(lookWanted_ && writes_ == 0 && look_ == Look::Idle)
    {
        lookWanted_ = false;
        look_ = Look::Refetching;
        ais_.fetchChildren(
            "current", 1,
            [this, generation = generation_](bool ok, const inventory::Model::Merged&)
            {
                if(generation != generation_)
                {
                    return;
                }
                const inventory::Folder* folder = model_.folderOfType(inventory::kCurrentOutfit);
                if(!ok || !folder)
                {
                    look_ = Look::Idle;
                    changing_.clear();
                    return;
                }
                askForLook(folder->version);
            });
    }

    // Attachments.
    std::vector<inventory::Ais::Link> made;
    std::erase_if(attaching_,
                  [&](const auto& entry)
                  {
                      const auto& [item, asked] = entry;
                      if(!worn.contains(item))
                      {
                          return now - asked > kTimeout;
                      }
                      const inventory::Item* target = model_.item(item);
                      if(target && linksTo(*cof, item).empty())
                      {
                          made.push_back(
                              {item, inventory::kLink, target->inventoryType, target->name, ""});
                      }
                      return true;
                  });
    if(!made.empty())
    {
        log::info("outfit: linking {} attachment(s) in the Current Outfit Folder", made.size());
        ais_.createLinks(cof->id, made, nullptr);
    }
    std::erase_if(detaching_,
                  [&](const auto& entry)
                  {
                      const auto& [item, asked] = entry;
                      if(worn.contains(item))
                      {
                          return now - asked > kTimeout;
                      }
                      for(const Uuid& link : linksTo(*cof, item))
                      {
                          log::info("outfit: unlinking {} from the Current Outfit Folder",
                                    item.toString());
                          ais_.removeItem(link, nullptr);
                      }
                      return true;
                  });
}

void CurrentOutfit::clear()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    attaching_.clear();
    detaching_.clear();
    changes_.clear();
    changing_.clear();
    outfit_.reset();
    outfitObjects_.clear();
    syncAttachments_ = false;
    writes_ = 0;
    lookWanted_ = false;
    look_ = Look::Idle;
    retries_ = 0;
    fetched_ = false;
    fetching_ = false;
    generation_++;
}

} // namespace viewer
