#pragma once

// Wearing the whole outfit: as the official viewer does after login
// (LLAppearanceMgr::updateAppearanceFromCOF, LLAttachmentsMgr), the
// objects linked in the Current Outfit Folder that aren't worn get
// attached, added to their points (so several can share one: a head and
// its hair on the skull, rigged clothes on a hand). The simulator itself
// brings back one attachment a point.
//
// Through AISv3 (inventory::Ais): the COF's links, each with its target
// embedded, in one request ("category/current/links"); targets not
// embedded are fetched on their own. Without AISv3 (other grids), the
// older capabilities: the COF found among the inventory root's folders
// (FetchInventoryDescendents2), its links, then their items
// (FetchInventory2). Once our avatar has been in a moment (the
// simulator's own restores in), the objects not worn go on, five a frame
// (more at once makes the simulator fail some, as the official viewer
// notes); a while later, any still missing are asked for again, once.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/inventory/Ais.hpp"
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

    // Once a session: looks for the outfit, through AISv3 if `ais` is
    // available, otherwise the older capabilities and the login's inventory
    // root.
    void start(inventory::Ais& ais, const inventory::Model& model, std::string fetchDescendents,
               std::string fetchItems, const Uuid& agent, const Uuid& inventoryRoot);
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
    // An inventory object as attaching takes it.
    static Item toAttach(const inventory::Item& item);

    static constexpr int kOutfitFolderType = 46; // FT_CURRENT_OUTFIT
    static constexpr int kLinkType = 24;         // AT_LINK
    static constexpr int kObjectType = 6;        // AT_OBJECT
    static constexpr double kSettle = 3.0;       // seconds in before attaching
    static constexpr double kQuiet = 2.0;        // ... and without new attachments
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
    // AISv3: the links' targets, those embedded or fetched.
    void linksArrived(bool ok, const inventory::Model::Merged& merged);
    void targetArrived(bool ok);
    void finishFromModel();

    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<net::HttpResponse>> replies_ =
        std::make_shared<MessageQueue<net::HttpResponse>>();
    std::vector<net::HttpResponse> drained_;
    State state_ = State::Idle;
    std::string fetchDescendents_;
    std::string fetchItems_;
    Uuid agent_;
    inventory::Ais* ais_ = nullptr;
    const inventory::Model* model_ = nullptr;
    std::vector<Uuid> links_;   // the COF's links (AISv3)
    int targetsAwaited_ = 0;    // link targets being fetched on their own
    std::vector<Item> outfit_;  // the objects in it
    std::vector<Item> pending_; // to attach, five at a time
    int passes_ = 0;            // attaching passes done (two at most)
    std::optional<double> nextPass_;
    double lastSent_ = -1e9;
    size_t wornCount_ = 0; // as last seen, and when it last changed
    double wornChanged_ = 0.0;
};

} // namespace viewer
