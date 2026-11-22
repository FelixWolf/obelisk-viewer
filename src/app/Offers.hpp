#pragma once

// What people and objects offer us, and what the grid tells us, waiting
// for an answer on the notice page (after scripts' dialogs): teleports
// offered (and asked for), friendship, group invitations, inventory given,
// group notices, web pages and messages. All come as instant messages
// (ImprovedInstantMessage, or ReadOfflineMsgs' records when they came
// while we were away). As the official viewer (llimprocessing.cpp,
// llviewermessage.cpp):
// - A teleport offered (22) is taken with TeleportLureRequest, declined
//   with an IM (24). A summons (25) is taken at once, no notice.
// - Asked to offer a teleport (26): StartLure to them; declined: nothing.
// - Friendship (38): AcceptFriendship (with the calling card folder) or
//   DeclineFriendship; if it came while we were away, by the
//   AcceptFriendship / DeclineFriendship capabilities.
// - A group invitation (3): an IM to the group (35, 36); if it came while
//   away with no transaction, by AcceptGroupInvite / DeclineGroupInvite.
//   The fee shows on the notice, which is the confirmation.
// - Inventory from someone (4) is already ours: it's accepted into its
//   type's folder when it comes (5); discarding moves it to the Trash.
// - Inventory from an object (9) is kept (10, into its type's folder) or
//   declined (11). (The official viewer takes a declined one into the
//   Trash; declining leaves it with the object.)
// - A group notice (32) with an attachment: kept (33) or not (34).
// No muting or do-not-disturb yet.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace viewer
{

namespace protocol::msg
{
struct ImprovedInstantMessage;
}
namespace session
{
class Session;
}

struct Offer
{
    enum class Kind : uint8_t
    {
        Teleport,        // a teleport offered
        TeleportRequest, // they ask us to offer one
        Friendship,
        GroupInvite,
        Inventory,       // from someone: already ours
        ObjectInventory, // from an object: kept or declined
        GroupNotice,
        Url,     // a web page from the grid
        Message, // from the grid, or an object's alert
    };
    Kind kind = Kind::Message;
    uint64_t serial = 0;  // unique while the viewer runs
    uint64_t region = 0;  // the region that sent it
    Uuid from;            // who (an object's owner; a group notice's sender, or group)
    std::string fromName; // their name (an object's: the object's)
    bool fromObject = false;
    Uuid transaction;     // what answers name (a lure's id, ...)
    bool offline = false; // came while we were away
    std::string message;
    // Teleport: the region's rating ("General", "Moderate", "Adult"), if said.
    std::string rating;
    // GroupInvite: the group and what joining costs (L$). GroupNotice: the group.
    Uuid group;
    int32_t fee = 0;
    // Inventory, ObjectInventory, a GroupNotice's attachment (assetType
    // -1: none): what it is, its name, and (from someone) the item.
    int assetType = -1;
    std::string itemName;
    Uuid item;
    // GroupNotice: the subject (the message is the rest).
    std::string subject;
    // Url: where.
    std::string url;
};

// What to do about an answer (one or more of these).
struct OfferAnswer
{
    // An instant message: to whom, its id (the offer's transaction), the
    // dialog, and (accepting inventory) the folder of this folder type in
    // its bucket (-1: the root; none: an empty bucket).
    struct Im
    {
        Uuid to;
        Uuid id;
        uint8_t dialog = 0;
        std::optional<int> folderType;
    };
    std::optional<Im> im;
    // Teleport by this lure.
    std::optional<Uuid> lure;
    bool godlike = false;
    // Offer a teleport to them.
    std::optional<Uuid> offerTeleportTo;
    // Friendship by message: accept (into the calling card folder) or not.
    std::optional<bool> friendship;
    Uuid transaction;
    // Move this item to the Trash.
    std::optional<Uuid> trash;
    // Open this web page.
    std::optional<std::string> url;
};

// Asset type n's folder type (the same number; llfoldertype.cpp).
inline int folderTypeOf(int assetType)
{
    return assetType;
}

class Offers
{
public:
    static constexpr size_t kMaxWaiting = 32; // the oldest go beyond this

    // `http` answers by capability (offers that came while away); none
    // (tests): those aren't answered.
    explicit Offers(net::HttpClient* http = nullptr);
    ~Offers();
    Offers(const Offers&) = delete;
    Offers& operator=(const Offers&) = delete;

    // From the region (`region`: its handle; 0 for what came while away).
    // False if it isn't an offer (a conversation's message, ...).
    bool handle(const protocol::msg::ImprovedInstantMessage& m, uint64_t region);

    // The one shown (the oldest waiting), and how many wait.
    const Offer* front() const { return waiting_.empty() ? nullptr : &waiting_.front(); }
    size_t waiting() const { return waiting_.size(); }
    // Changes whenever what's waiting does.
    uint64_t version() const { return version_; }

    // Answers to the one shown, which goes: its first button (Teleport,
    // Accept, Join, Keep, Open, OK) or the other (Decline, Discard, Close).
    void accept();
    void decline();

    // What to send, once (answers, and what's done as offers come).
    std::vector<OfferAnswer> takeAnswers() { return std::exchange(answers_, {}); }

    // Sends what goes by capability. Once a frame.
    void update(const session::Session& session);

    // Logged out: nothing waits.
    void clear();

private:
    void add(Offer offer);
    void answer(const Offer& offer, bool accepted);

    std::deque<Offer> waiting_;
    uint64_t serial_ = 0;
    uint64_t version_ = 0;
    std::vector<OfferAnswer> answers_;

    // Capability answers: friendship (from), group invitations (group).
    struct ByCapability
    {
        enum class Kind : uint8_t
        {
            Friendship,
            Group,
        };
        Kind kind = Kind::Friendship;
        bool accept = false;
        Uuid id;
    };
    std::vector<ByCapability> byCapability_;
    net::HttpClient* http_ = nullptr;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    struct Reply
    {
        std::string what;
        net::HttpResponse response;
    };
    std::shared_ptr<MessageQueue<Reply>> replies_ = std::make_shared<MessageQueue<Reply>>();
};

} // namespace viewer
