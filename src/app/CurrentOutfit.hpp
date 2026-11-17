#pragma once

// Changing what we wear (the main menu's inventory), through the Current
// Outfit Folder, as the official viewer's LLAppearanceMgr does:
//
//  - Attachments: once one asked for has arrived (the simulator attaches
//    it), a link to it goes into the COF, so the next login wears it
//    again; once one asked off has gone, its links come out. Only what we
//    ask for here: what the simulator brings back or takes off on its own
//    isn't changed in the COF.
//  - Clothing and body parts: the COF is what's worn. Wearing one links it
//    (a body part, or clothing worn rather than added, replaces the links
//    to others of its kind: shape, skin, shirt, ...); taking clothing off
//    removes its links. Then the region is asked to make our look from the
//    COF (UpdateAvatarAppearance, with the COF's version, asked for again
//    after the changes; the simulator answers "expected" when the version
//    is stale, and is asked again with that one). Our new look comes as
//    AvatarAppearance, as anyone's.
//
// The COF and its links are fetched once (AISv3's "current" alias) before
// any change, so links aren't made twice.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/inventory/Ais.hpp"
#include "viewer/inventory/Model.hpp"
#include "viewer/net/HttpClient.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer
{

class CurrentOutfit
{
public:
    // An attachment given up on after this long (seconds): the simulator
    // didn't do it.
    static constexpr double kTimeout = 30.0;
    // Asked again with the version the simulator expected, this many times.
    static constexpr int kRetries = 3;

    // Clothing and body parts (llassettype.h's AT_CLOTHING, AT_BODYPART).
    static constexpr int kClothing = 5;
    static constexpr int kBodyPart = 13;
    static bool isWearable(const inventory::Item& item)
    {
        return item.type == kClothing || item.type == kBodyPart;
    }
    // Shape, skin, shirt, ... (llwearabletype.h): an item's flags' low byte.
    static int wearableType(const inventory::Item& item) { return int(item.flags & 0xff); }

    CurrentOutfit(net::HttpClient& http, inventory::Ais& ais, inventory::Model& model);
    ~CurrentOutfit();

    // The region's UpdateAvatarAppearance (empty: none, no look asked for).
    void setAppearanceCapability(std::string url) { appearanceUrl_ = std::move(url); }

    // We asked for an item to be attached, or taken off.
    void attaching(const Uuid& item, double now);
    void detaching(const Uuid& item, double now);

    // Clothing or a body part: worn instead of others of its kind (always,
    // for body parts), or added to them (clothing); clothing taken off.
    enum class How
    {
        Replace,
        Add,
    };
    void wear(const Uuid& item, How how);
    void takeOff(const Uuid& item);

    // Something asked of an item isn't done yet.
    bool pending(const Uuid& item) const;
    // A wearable linked in the COF (false until the COF is known; asked for).
    bool wearing(const Uuid& item);

    // What's attached now (attachments' items): links made or removed for
    // those that arrived or went; wearables' changes sent, and our look
    // asked for once they're in. Each frame or so.
    void update(const std::unordered_set<Uuid>& worn, double now);
    void clear();

private:
    // The COF, once it and its links are known (asked for if not).
    const inventory::Folder* outfitFolder();
    // The COF's links to an item.
    std::vector<Uuid> linksTo(const inventory::Folder& cof, const Uuid& item) const;
    void changeWearables(const inventory::Folder& cof);
    void askForLook(int version);

    struct Change
    {
        Uuid item;
        bool on = true;
        How how = How::Replace;
    };
    // Where asking for our look is.
    enum class Look
    {
        Idle,
        Refetching, // the COF, for its version after the changes
        Asking,     // UpdateAvatarAppearance
    };
    struct LookAnswer
    {
        uint64_t generation = 0;
        net::HttpResponse response;
    };

    net::HttpClient& http_;
    inventory::Ais& ais_;
    inventory::Model& model_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<LookAnswer>> answers_ =
        std::make_shared<MessageQueue<LookAnswer>>();
    std::string appearanceUrl_;

    std::unordered_map<Uuid, double> attaching_; // item: when asked
    std::unordered_map<Uuid, double> detaching_;
    std::vector<Change> changes_;       // wearables', to send
    std::unordered_set<Uuid> changing_; // ... and those sent, until answered
    int writes_ = 0;                    // COF writes not answered yet
    bool lookWanted_ = false;
    Look look_ = Look::Idle;
    int retries_ = 0;
    bool fetched_ = false;
    bool fetching_ = false;
    uint64_t generation_ = 0;
};

} // namespace viewer
