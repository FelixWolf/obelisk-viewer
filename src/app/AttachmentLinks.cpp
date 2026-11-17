#include "AttachmentLinks.hpp"

#include "viewer/core/Log.hpp"

#include <vector>

namespace viewer
{

void AttachmentLinks::attaching(const Uuid& item, double now)
{
    detaching_.erase(item);
    attaching_[item] = now;
}

void AttachmentLinks::detaching(const Uuid& item, double now)
{
    attaching_.erase(item);
    detaching_[item] = now;
}

const inventory::Folder* AttachmentLinks::outfitFolder()
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

void AttachmentLinks::update(const std::unordered_set<Uuid>& worn, double now)
{
    if(attaching_.empty() && detaching_.empty())
    {
        return;
    }
    const inventory::Folder* cof = outfitFolder();
    if(!cof)
    {
        return; // once it's known
    }
    // The COF's links to an item.
    const auto links = [&](const Uuid& item)
    {
        std::vector<Uuid> found;
        for(const inventory::Item* i : model_.items(cof->id))
        {
            if(i->isLink() && i->linkedId == item)
            {
                found.push_back(i->id);
            }
        }
        return found;
    };

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
                      if(target && links(item).empty())
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
                      for(const Uuid& link : links(item))
                      {
                          log::info("outfit: unlinking {} from the Current Outfit Folder",
                                    item.toString());
                          ais_.removeItem(link, nullptr);
                      }
                      return true;
                  });
}

void AttachmentLinks::clear()
{
    attaching_.clear();
    detaching_.clear();
    fetched_ = false;
    fetching_ = false;
    generation_++;
}

} // namespace viewer
