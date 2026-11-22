#pragma once

// Instant messages: conversations with one person, and group and ad-hoc
// chats, as the official viewer keeps them (llimprocessing.cpp, llimview.cpp):
// - One to one (ImprovedInstantMessage, dialog 0): the session is the two
//   agents' ids XORed; what we send shows at once (the region doesn't send
//   it back). Typing starts and stops (41, 42) and do-not-disturb answers
//   (20) go to the same conversation.
// - Group and ad-hoc chats: the first message comes as an invitation on the
//   event queue (ChatterBoxInvitation), which we accept (ChatSessionRequest
//   "accept invitation"); later ones as dialog 17 (IM_SESSION_SEND), ours
//   among them (sent to the session, it sends them back to everyone).
// - Objects' messages (llInstantMessage, dialog 19) aren't conversations:
//   they go to local chat, as said to us alone.
// - What came while we were away is read once after logging in
//   (ReadOfflineMsgs), marked as saved.
// Offers (teleports, inventory, friendship, groups) are Offers'.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

// ImprovedInstantMessage's Dialog values used here (EInstantMessage).
namespace ImDialog
{
inline constexpr uint8_t NothingSpecial = 0; // one to one
inline constexpr uint8_t MessageBox = 1;     // from the grid
inline constexpr uint8_t GroupInvitation = 3;
inline constexpr uint8_t InventoryOffered = 4; // by someone
inline constexpr uint8_t InventoryAccepted = 5;
inline constexpr uint8_t InventoryDeclined = 6;
inline constexpr uint8_t TaskInventoryOffered = 9; // by an object
inline constexpr uint8_t TaskInventoryAccepted = 10;
inline constexpr uint8_t TaskInventoryDeclined = 11;
inline constexpr uint8_t SessionSend = 17; // a group or ad-hoc chat's message
inline constexpr uint8_t FromTask = 19;    // an object's (llInstantMessage)
inline constexpr uint8_t BusyAutoResponse = 20;
inline constexpr uint8_t LureUser = 22; // a teleport offered
inline constexpr uint8_t LureAccepted = 23;
inline constexpr uint8_t LureDeclined = 24;
inline constexpr uint8_t GodlikeLureUser = 25; // a summons
inline constexpr uint8_t TeleportRequest = 26; // asking to be offered one
inline constexpr uint8_t GotoUrl = 28;
inline constexpr uint8_t FromTaskAsAlert = 31;
inline constexpr uint8_t GroupNotice = 32;
inline constexpr uint8_t GroupNoticeInventoryAccepted = 33;
inline constexpr uint8_t GroupNoticeInventoryDeclined = 34;
inline constexpr uint8_t GroupInvitationAccept = 35;
inline constexpr uint8_t GroupInvitationDecline = 36;
inline constexpr uint8_t GroupNoticeRequested = 37;
inline constexpr uint8_t FriendshipOffered = 38;
inline constexpr uint8_t FriendshipAccepted = 39;
inline constexpr uint8_t TypingStart = 41;
inline constexpr uint8_t TypingStop = 42;
} // namespace ImDialog

struct ImMessage
{
    std::chrono::system_clock::time_point when{};
    Uuid from;
    std::string fromName; // "First" or "First Last" (no " Resident")
    std::string text;
    bool ours = false;
    bool saved = false;    // sent while we were away
    bool response = false; // an automatic answer (do not disturb)
};

struct ImConversation
{
    enum class Kind : uint8_t
    {
        Direct,  // one to one
        Session, // a group or ad-hoc chat
    };
    Uuid session;
    Kind kind = Kind::Direct;
    Uuid other;       // one to one: the other person
    std::string name; // the other person's, or the chat's (a group's name)
    std::deque<ImMessage> lines;
    uint64_t added = 0;  // lines ever added (changes with each)
    bool typing = false; // the other person is typing (one to one)
    std::chrono::system_clock::time_point latest{};
};

// The one-to-one session with someone (ids XORed; ours alone: our id).
Uuid directSession(const Uuid& self, const Uuid& other);

class InstantMessages
{
public:
    static constexpr size_t kMaxLines = 500; // a conversation's, the oldest go
    static constexpr size_t kMaxText = 1023; // what's sent, in bytes (whole characters)

    // `http` asks for what came while away and accepts invitations; none
    // (tests): neither.
    explicit InstantMessages(net::HttpClient* http = nullptr);
    ~InstantMessages();
    InstantMessages(const InstantMessages&) = delete;
    InstantMessages& operator=(const InstantMessages&) = delete;

    // An object's message, for local chat.
    struct FromObject
    {
        Uuid object;
        Uuid owner;
        std::string name;
        std::string text;
    };
    // From the region. `self`: our agent id; `now` the wall clock.
    std::optional<FromObject> handle(const protocol::msg::ImprovedInstantMessage& m,
                                     const Uuid& self, std::chrono::system_clock::time_point now);
    // ChatterBoxInvitation's body: the chat it starts (to accept), if any.
    std::optional<Uuid> invitation(const llsd::Value& body, const Uuid& self,
                                   std::chrono::system_clock::time_point now);
    // ReadOfflineMsgs' answer: what came while away; objects' messages for
    // local chat.
    std::vector<FromObject> offline(const llsd::Value& reply, const Uuid& self);

    // What to send to say `text` in a conversation (nothing if it's not
    // known or there's nothing to say). One to one, it shows at once.
    struct Outgoing
    {
        Uuid to; // the other person, or the session (group and ad-hoc)
        Uuid session;
        uint8_t dialog = ImDialog::NothingSpecial;
        std::string text;
    };
    std::optional<Outgoing> send(const Uuid& session, std::string_view text, const Uuid& self,
                                 std::string_view ourName,
                                 std::chrono::system_clock::time_point now);

    // Asks for what came while away (once a login, when the region has the
    // capability), accepts invitations, and takes the answers. Once a frame.
    void update(const session::Session& session);

    // Objects' messages that came while away, for local chat, once.
    std::vector<FromObject> takeOfflineFromObjects()
    {
        return std::exchange(offlineFromObjects_, {});
    }
    // The rest of what came while away (offers and the like), as the
    // region would have sent them, once.
    std::vector<protocol::msg::ImprovedInstantMessage> takeOfflineOthers();

    // Newest activity first.
    const std::vector<ImConversation>& conversations() const { return conversations_; }
    const ImConversation* find(const Uuid& session) const;
    // Changes whenever anything shown does.
    uint64_t version() const { return version_; }

    // Logged out: nothing kept.
    void clear();

private:
    ImConversation& conversation(const Uuid& session, ImConversation::Kind kind, const Uuid& other,
                                 std::string_view name);
    void add(ImConversation& c, ImMessage message);
    void sortByLatest();

    std::vector<ImConversation> conversations_;
    uint64_t version_ = 0;

    // Capability requests.
    net::HttpClient* http_ = nullptr;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    struct Answer
    {
        uint64_t generation = 0;
        bool offline = false; // ReadOfflineMsgs' (else an invitation accepted)
        Uuid session;
        net::HttpResponse response;
    };
    std::shared_ptr<MessageQueue<Answer>> answers_ = std::make_shared<MessageQueue<Answer>>();
    uint64_t generation_ = 1; // clear() forgets answers on their way
    bool offlineAsked_ = false;
    std::vector<Uuid> toAccept_;
    std::vector<FromObject> offlineFromObjects_;
    std::vector<protocol::msg::ImprovedInstantMessage> offlineOthers_;
};

// ReadOfflineMsgs' record of a message as the region sends it (Offline set).
protocol::msg::ImprovedInstantMessage offlineMessage(const llsd::Value& record);

} // namespace viewer
