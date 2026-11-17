#pragma once

// Keeps the Current Outfit Folder in step with what we attach and detach
// (the main menu's inventory), as the official viewer's LLAppearanceMgr and
// LLAttachmentsMgr do: once an attachment asked for has arrived, a link to
// it goes into the COF (so the next login wears it again); once one asked
// off has gone, its links come out. The COF and its links are fetched once
// (AISv3's "current" alias) before any change, so links aren't made twice.
//
// Only what we ask for here: attachments the simulator brings back or
// takes off on its own aren't changed in the COF.

#include "viewer/core/Uuid.hpp"
#include "viewer/inventory/Ais.hpp"
#include "viewer/inventory/Model.hpp"

#include <unordered_map>
#include <unordered_set>

namespace viewer
{

class AttachmentLinks
{
public:
    // Given up on after this long (seconds): the simulator didn't do it.
    static constexpr double kTimeout = 30.0;

    AttachmentLinks(inventory::Ais& ais, inventory::Model& model) : ais_(ais), model_(model) {}

    // We asked for an item to be attached, or taken off.
    void attaching(const Uuid& item, double now);
    void detaching(const Uuid& item, double now);
    bool pending(const Uuid& item) const
    {
        return attaching_.contains(item) || detaching_.contains(item);
    }

    // What's worn now (attachments' items): links made or removed for those
    // that arrived or went.
    void update(const std::unordered_set<Uuid>& worn, double now);
    void clear();

private:
    // The COF, once it and its links are known (asked for if not).
    const inventory::Folder* outfitFolder();

    inventory::Ais& ais_;
    inventory::Model& model_;
    std::unordered_map<Uuid, double> attaching_; // item: when asked
    std::unordered_map<Uuid, double> detaching_;
    bool fetched_ = false;
    bool fetching_ = false;
    uint64_t generation_ = 0;
};

} // namespace viewer
