#pragma once

// Wearing the whole outfit: as the official viewer does after login
// (LLAppearanceMgr::updateAppearanceFromCOF, LLAttachmentsMgr), the
// objects linked in the Current Outfit Folder that aren't worn get
// attached, added to their points (so several can share one: a head and
// its hair on the skull, rigged clothes on a hand). The simulator itself
// brings back one attachment a point.
//
// The COF is found among the inventory root's folders (its type, 46),
// fetched (FetchInventoryDescendents2: links), and the linked items
// fetched (FetchInventory2) for what attaching needs (owner, permission
// masks, name). Once our avatar has been in a while (the simulator's own
// restores in), the objects not worn go on, five at a time (more at once
// makes the simulator fail some, as the official viewer notes); a while
// later, any still missing are asked for again, once.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/session/Session.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace viewer
{

class OutfitAttachments
{
public:
    using Item = session::Session::InventoryObject;

    explicit OutfitAttachments(net::HttpClient& http);

    // Once a session: looks for the outfit (the capabilities and the
    // login's inventory root).
    void start(std::string fetchDescendents, std::string fetchItems, const Uuid& agent,
               const Uuid& inventoryRoot);
    // Each frame: handles replies; `arrived` is how long our avatar has
    // been in (seconds; none: not yet), `worn` the items attached now.
    // What to attach now (at most five).
    std::vector<Item> update(double now, std::optional<double> arrived,
                             const std::unordered_set<Uuid>& worn);
    bool started() const { return state_ != State::Idle; }
    // Still to do something with what's worn (attaching passes left).
    bool busy() const { return state_ != State::Failed && (passes_ < 2 || !pending_.empty()); }
    void clear();

    // Parsing (tests): the Current Outfit Folder among a folder's
    // (FetchInventoryDescendents2 reply); the items its links point at;
    // the objects among fetched items (FetchInventory2 reply).
    static std::optional<Uuid> outfitFolder(const llsd::Value& reply);
    static std::vector<Uuid> linkedItems(const llsd::Value& reply);
    static std::vector<Item> objects(const llsd::Value& reply);

    static constexpr int kOutfitFolderType = 46; // FT_CURRENT_OUTFIT
    static constexpr int kLinkType = 24;         // AT_LINK
    static constexpr int kObjectType = 6;        // AT_OBJECT
    static constexpr double kSettle = 8.0;       // seconds in before attaching
    static constexpr double kRetry = 30.0;       // then again for those missing
    static constexpr size_t kPerRequest = 5;

private:
    enum class State : uint8_t
    {
        Idle,
        FindingFolder,
        FetchingOutfit,
        FetchingItems,
        Ready,
        Failed,
    };
    void post(const std::string& url, const llsd::Value& body);
    void fetchFolder(const Uuid& folder, bool folders, bool items);

    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<net::HttpResponse>> replies_ =
        std::make_shared<MessageQueue<net::HttpResponse>>();
    std::vector<net::HttpResponse> drained_;
    State state_ = State::Idle;
    std::string fetchDescendents_;
    std::string fetchItems_;
    Uuid agent_;
    std::vector<Item> outfit_;  // the objects in it
    std::vector<Item> pending_; // to attach, five at a time
    int passes_ = 0;            // attaching passes done (two at most)
    std::optional<double> nextPass_;
    double lastSent_ = -1e9;
};

} // namespace viewer
