#pragma once

// Everything that belongs to one login. The App holds a
// std::unique_ptr<Session>: logging in creates one, logging out (or a failed
// login) destroys it, and logging in again creates a fresh one, all without
// restarting the viewer.
//
// A session talks to several simulators at once: the one the agent is in
// (the main region) and its neighbours, which the main region announces
// through the event queue (EnableSimulator). Each one is a Region with its own
// UDP circuit, capabilities and event queue poller.
//
// The main region changes when the agent teleports (TeleportFinish) or walks
// into a neighbour (CrossedRegion): the destination becomes the main region at
// once and the agent arrives there with AgentMovementComplete (see
// arrivals()). Regions the destination doesn't need are closed by their
// simulators (DisableSimulator) or go silent.
//
// Threading: HTTP callbacks run on the HttpClient's I/O thread and the UDP
// circuits on the CircuitManager's network thread; both only queue what
// arrived. All state changes happen in update(), on the thread that owns the
// Session (currently the main thread).
//
// Lifetime: destroying a Session cancels its HTTP requests and closes its
// circuits, so it can be destroyed at any time. Call requestLogout() first
// (and keep calling update() until state() is LoggedOut) to tell the grid.
// The HttpClient must outlive the Session.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/net/UdpSocket.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/session/Login.hpp"

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::session
{

enum class SessionState : uint8_t
{
    LoggingIn,  // waiting for the login server
    Connecting, // circuit and capabilities for the main region
    Connected,  // in the world
    LoggingOut, // LogoutRequest sent, waiting for LogoutReply
    LoggedOut,  // done; the owner can destroy the Session
    Failed,
};

std::string_view stateName(SessionState state);

enum class TeleportState : uint8_t
{
    None,
    LookingUp,  // asking the map for the region's handle (teleport by name)
    Requested,  // request sent; waiting for the simulator to start
    InProgress, // TeleportStart: the simulator is moving us
    Arriving,   // TeleportFinish: connecting to the destination
};

std::string_view teleportStateName(TeleportState state);

struct TeleportStatus
{
    TeleportState state = TeleportState::None;
    std::string message{}; // the simulator's latest progress message
    std::string failure{}; // why the last teleport failed; empty if it didn't
};

// The agent arrived somewhere: logged in, teleported, walked into another
// region, or teleported within its region.
struct Arrival
{
    enum class Kind : uint8_t
    {
        Login,
        Teleport,
        Crossing,
        LocalTeleport,
    };
    Kind kind = Kind::Login;
    uint64_t region = 0; // handle
    protocol::Vector3 position{};
    protocol::Vector3 lookAt{1, 0, 0};
};

// AgentUpdate control flags (as indra_constants.h's AGENT_CONTROL_*).
namespace control
{
inline constexpr uint32_t kAtPos = 1u << 0; // forward
inline constexpr uint32_t kAtNeg = 1u << 1;
inline constexpr uint32_t kLeftPos = 1u << 2; // strafe left
inline constexpr uint32_t kLeftNeg = 1u << 3;
inline constexpr uint32_t kUpPos = 1u << 4; // jump / fly up
inline constexpr uint32_t kUpNeg = 1u << 5;
inline constexpr uint32_t kYawPos = 1u << 8; // turning left (the body rotation does the turning)
inline constexpr uint32_t kYawNeg = 1u << 9;
inline constexpr uint32_t kFastAt = 1u << 10;
inline constexpr uint32_t kFastLeft = 1u << 11;
inline constexpr uint32_t kFastUp = 1u << 12;
inline constexpr uint32_t kFly = 1u << 13;
inline constexpr uint32_t kStop = 1u << 14;
inline constexpr uint32_t kStandUp = 1u << 16;
inline constexpr uint32_t kMouselook = 1u << 18;     // first person
inline constexpr uint32_t kMouselookDown = 1u << 30; // the left button, in first person
inline constexpr uint32_t kMouselookUp = 1u << 31;
} // namespace control

// Capability names requested from each region's seed. Regions grant a subset.
std::vector<std::string> defaultCapabilityNames();

// AgentThrottle's bandwidth per category (resend, land, wind, cloud, task,
// texture, asset), in bits per second, for a total in kilobits per second,
// as the official viewer splits it (llviewerthrottle.cpp: its presets for
// 50, 300, 500 and 1000 kbps, interpolated between, extrapolated beyond).
std::array<float, 7> throttleBitsPerSecond(float kbps);

// The official viewer's default bandwidth setting (ThrottleBandwidthKBPS).
inline constexpr float kDefaultBandwidthKbps = 3000.0f;

// How much of the bandwidth setting to ask for, as the official viewer's
// dynamic throttle (llviewerthrottle.cpp): up to 1.5 times the setting
// (regions rarely fill every category at once), stepping down by 0.1 (to
// 0.2 at least) while incoming packets are lost, back up when they aren't.
class DynamicThrottle
{
public:
    static constexpr float kMaxFraction = 1.5f;
    static constexpr float kMinFraction = 0.2f;
    static constexpr float kStep = 0.1f;
    static constexpr float kTightenLossPercent = 3.0f; // lost of what arrived, per check
    static constexpr float kEaseLossPercent = 0.5f;
    static constexpr float kMinKbps = 50.0f;
    static constexpr float kMaxKbps = 6000.0f;

    // The setting (kilobits per second); back to the most of it.
    void setBandwidth(float kbps);
    float bandwidth() const { return bandwidth_; }
    // What to ask for now, kilobits per second.
    float current() const { return bandwidth_ * fraction_; }
    // Packets that arrived and were lost since the last check; true if
    // current() changed (the region needs new throttles).
    bool update(uint64_t arrived, uint64_t lost);

private:
    float bandwidth_ = kDefaultBandwidthKbps;
    float fraction_ = kMaxFraction;
};

using Capabilities = std::map<std::string, std::string, std::less<>>;

// One simulator we have a circuit to.
struct Region
{
    protocol::CircuitId circuit = 0;
    net::Endpoint endpoint{};
    uint64_t handle = 0; // (global x in meters << 32) | global y in meters
    bool isMain = false;

    // From RegionHandshake.
    bool handshakeReceived = false;
    // Waiting for replyToHandshake() (see deferHandshakeReplies()) since then.
    std::optional<std::chrono::steady_clock::time_point> handshakeReplyDue{};
    std::string name{};
    Uuid regionId{};
    uint32_t flags = 0;
    static constexpr uint32_t kAllowVoice = 1u << 28; // REGION_FLAGS_ALLOW_VOICE

    // From SimStats (the official LL_SIM_STAT_*: 0, 1, 2), about every
    // second; nothing before the first.
    struct Stats
    {
        float timeDilation = 1.0f;
        float fps = 0.0f;
        float physicsFps = 0.0f;
    };
    std::optional<Stats> stats{};

    std::string seedCapability{}; // empty until the region gives us one
    Capabilities capabilities{};
    bool capabilitiesReceived = false;
    int seedAttempts = 0;
    std::optional<std::chrono::steady_clock::time_point> seedRetryAt{};

    bool eventQueueRunning = false;
    // Bumped when the region's HTTP requests are replaced (a new seed):
    // replies to the old ones are dropped.
    uint32_t httpGeneration = 0;
    llsd::Value eventQueueAck{}; // id of the last batch we received
    std::optional<std::chrono::steady_clock::time_point> eventQueueRetryAt{};

    // This region's HTTP requests (seed, event queue); destroying the
    // Region cancels them.
    std::unique_ptr<net::HttpRequestGroup> http{};

    uint32_t gridX() const { return static_cast<uint32_t>(handle >> 32) / 256; }
    uint32_t gridY() const { return static_cast<uint32_t>(handle) / 256; }
};

// A message for the rest of the viewer (world state etc.), tagged with the
// region it came from. Session handles the connection-level messages itself
// and passes everything along.
struct RegionMessage
{
    enum class Kind : uint8_t
    {
        Message,       // `event` holds a message from the region
        RegionRemoved, // the region's circuit was closed
    };
    Kind kind = Kind::Message;
    uint64_t regionHandle = 0;
    protocol::CircuitEvent event{};
};

// An event from a region's event queue the session doesn't use itself
// (ParcelProperties, ...): the app's.
struct RegionEvent
{
    uint64_t regionHandle = 0;
    std::string name;
    llsd::Value body;
};

class Session
{
public:
    // Starts logging in immediately.
    Session(net::HttpClient& http, LoginParameters parameters);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Processes everything that arrived and runs timers. Call every frame.
    void update();

    // Tells the main region we're leaving. update() then moves to LoggedOut
    // when it replies (or after a timeout). Before we're in the world, it
    // just stops.
    void requestLogout();

    SessionState state() const { return state_; }

    // Messages for the world, in arrival order. Drain every frame; they
    // queue up otherwise.
    void drainRegionMessages(std::vector<RegionMessage>& out)
    {
        out.clear();
        std::swap(out, regionMessages_);
    }
    // Events for the app, in arrival order; drain every frame too.
    void drainRegionEvents(std::vector<RegionEvent>& out)
    {
        out.clear();
        std::swap(out, regionEvents_);
    }

    // Where the agent is: its region's handle and the position in it
    // (from AgentMovementComplete and TeleportLocal).
    std::optional<uint64_t> agentRegion() const { return agentRegion_; }
    protocol::Vector3 agentPosition() const { return agentPosition_; }

    // Counts arrivals (AgentMovementComplete, TeleportLocal); lastArrival()
    // is the latest. Compare the count to notice new ones.
    uint64_t arrivals() const { return arrivals_; }
    const Arrival& lastArrival() const { return lastArrival_; }

    // Our L$ (MoneyBalanceReply: asked for on arriving at login, then sent
    // whenever it changes). Nothing: not heard yet.
    std::optional<int32_t> balance() const { return balance_; }

    // Teleports. Ignored unless Connected and no teleport is under way; the
    // outcome shows in teleport() and, on success, arrivals().
    // By region name: the map gives us its handle first.
    void teleportTo(const Location& location);
    // To a region by handle, `position` in its coordinates.
    void teleportTo(uint64_t regionHandle, protocol::Vector3 position);
    void teleportHome();
    const TeleportStatus& teleport() const { return teleport_; }

    // The world map. Its answers come with the world's messages
    // (drainRegionMessages): MapBlockReply for regions whose names start
    // with `name` (where they are), MapItemReply (ItemType kMapAgentLocations)
    // for where everyone on the grid is, a few places a region with how
    // many at each (Extra). Ignored unless Connected.
    void requestMapRegion(std::string_view name);
    void requestAgentLocations(uint64_t regionHandle);
    static constexpr uint32_t kMapAgentLocations = 6; // MAP_ITEM_AGENT_LOCATIONS

    // Groups' names (UUIDGroupNameReply), and the main region's estate
    // (EstateCovenantReply: its name, owner and covenant), with the world's
    // messages. Ignored unless Connected.
    void requestGroupNames(std::span<const Uuid> groups);
    void requestEstateCovenant();

    // How the agent wants to move: control::* flags and the direction the
    // body faces. Sent to the main region at once when they change, and
    // repeated while moving.
    void setControls(uint32_t flags, protocol::Quaternion bodyRotation);

    // Downstream bandwidth to ask the regions for, kilobits per second (the
    // official viewer's ThrottleBandwidthKBPS); adjusted to packet loss
    // from there (DynamicThrottle). Takes effect at once.
    void setBandwidth(float kbps);
    // What we ask for now, kilobits per second.
    float currentBandwidth() const { return throttle_.current(); }

    // Object caches. The handshake reply tells a region whether we have its
    // objects cached; after deferHandshakeReplies() the session waits for
    // replyToHandshake() (up to 3 s, then replies "no cache").
    void deferHandshakeReplies() { deferHandshakeReplies_ = true; }
    void replyToHandshake(uint64_t regionHandle, bool haveObjectCache);
    // Asks the region for these objects in full: (local id, CacheMissType)
    // pairs, as RequestMultipleObjects.
    void requestObjects(uint64_t regionHandle,
                        std::span<const std::pair<uint32_t, uint8_t>> objects);

    // Touching an object (as the official viewer's ObjectGrab, then
    // ObjectDeGrab when it's let go): the prim touched and where, which
    // scripts read with llDetectedTouchUV, llDetectedTouchST,
    // llDetectedTouchFace, llDetectedTouchPos, llDetectedTouchNormal and
    // llDetectedTouchBinormal. `grabOffset`: from the prim to its link
    // set's root, in the root's frame. Positions are in the agent's region's
    // coordinates.
    struct TouchSurface
    {
        protocol::Vector3 uv{}; // texture coordinates (after the face's repeats)
        protocol::Vector3 st{}; // the face's own 0..1 coordinates
        int32_t face = -1;
        protocol::Vector3 position{};
        protocol::Vector3 normal{};
        protocol::Vector3 binormal{};
        bool operator==(const TouchSurface&) const = default;
    };
    void touchObject(uint64_t regionHandle, uint32_t localId, protocol::Vector3 grabOffset,
                     const TouchSurface& surface);
    void releaseObject(uint64_t regionHandle, uint32_t localId, const TouchSurface& surface);
    // While it's held: where on it the pointer is now (ObjectGrabUpdate;
    // face -1: off it), for touch events' llDetectedTouch* (sliders, colour
    // wheels). `grabPosition` in the region's coordinates, `sinceLast` in
    // milliseconds.
    void dragObject(uint64_t regionHandle, const Uuid& object, protocol::Vector3 grabOffset,
                    protocol::Vector3 grabPosition, uint32_t sinceLast,
                    const TouchSurface& surface);

    // Sits on an object (AgentRequestSit), `offset` where on it (the
    // object's frame), as the official viewer's "Sit Here"; the region
    // answers (AvatarSitResponse), and is told to go ahead (AgentSit).
    // Standing up is a control flag (control::kStandUp).
    void requestSit(uint64_t regionHandle, const Uuid& object, protocol::Vector3 offset);

    // Local chat (ChatFromViewer) to the main region: on a channel (0 is
    // what people hear), whispered (0), said (1) or shouted (2); 4 and 5
    // tell others we started or stopped typing.
    void say(std::string_view text, int32_t channel = 0, uint8_t type = 1);
    // An instant message (ImprovedInstantMessage) to `to` (a person, or a
    // group or ad-hoc chat's session) in `session`, as the official
    // pack_instant_message: from our name, online, with this binary
    // bucket (none: an empty one).
    void sendInstantMessage(const Uuid& to, const Uuid& session, uint8_t dialog,
                            std::string_view text, std::span<const uint8_t> bucket = {});

    // Offers: teleporting by a lure offered (TeleportLureRequest; as
    // teleportTo, ignored unless Connected and not teleporting), offering
    // ours (StartLure), answering friendship (AcceptFriendship with the
    // calling card folder, or DeclineFriendship) and moving an item to a
    // folder (MoveInventoryItem: the Trash, for one discarded).
    void teleportViaLure(const Uuid& lure, bool godlike);
    void offerTeleport(const Uuid& to, std::string_view message);
    void answerFriendship(const Uuid& transaction, bool accept, const Uuid& callingCards);
    void moveInventoryItem(const Uuid& item, const Uuid& folder);
    // Friends: ending a friendship (TerminateFriendship), and what we let
    // a friend do (GrantUserRights: FriendRights bits; the region answers
    // with ChangeUserRights).
    void endFriendship(const Uuid& other);
    void grantRights(const Uuid& other, int32_t rights);

    // Answers to scripts, to the region that asked (gone: not sent; the
    // script's object went with it). A dialog's button (ScriptDialogReply:
    // its index and label; a text box's text with index 0), and the
    // permissions granted (ScriptAnswerYes; 0: none).
    void replyToScriptDialog(uint64_t regionHandle, const Uuid& object, int32_t channel,
                             int32_t button, std::string_view label);
    void answerScriptPermissions(uint64_t regionHandle, const Uuid& task, const Uuid& item,
                                 int32_t granted);

    // Wears objects from inventory (RezMultipleAttachmentsFromInv), as the
    // official viewer's LLAttachmentsMgr: each at the point it was last
    // worn on, added to what's there (ATTACHMENT_ADD) rather than replacing
    // it, so several can share a point. One compound request.
    struct InventoryObject
    {
        Uuid itemId;
        Uuid ownerId;
        uint32_t flags = 0;
        uint32_t groupMask = 0;
        uint32_t everyoneMask = 0;
        uint32_t nextOwnerMask = 0;
        std::string name;
        std::string description;
    };
    static constexpr uint8_t kAttachmentAdd = 0x80;
    void attachFromInventory(std::span<const InventoryObject> items);
    // Takes off our attachments (their objects in the main region; ObjectDetach):
    // back into inventory.
    void detach(std::span<const uint32_t> localIds);

    // Our look-at and point-at (the official LLHUDEffectLookAt and
    // LLHUDEffectPointAt) as a ViewerEffect to the main region, which
    // passes it to viewers nearby: they turn our avatar's head and arm.
    struct AttentionEffect
    {
        Uuid id;               // the effect's own id (the same for every update)
        uint8_t effectType;    // kLookAtEffect or kPointAtEffect
        uint8_t targetType;    // what kind of look or point (the official enums)
        float duration = 0.0f; // seconds it lasts at the receivers
        Uuid target;           // what at (null: `position` is global)
        // Global meters, or an offset from the target (in its frame; for an
        // avatar, from its head).
        std::array<double, 3> position{};
    };
    static constexpr uint8_t kLookAtEffect = 14;  // LL_HUD_EFFECT_LOOKAT
    static constexpr uint8_t kPointAtEffect = 15; // LL_HUD_EFFECT_POINTAT
    void sendAttention(const AttentionEffect& effect);
    // Any ViewerEffect to the main region (passed to viewers nearby): its
    // own id, type, how long it lasts there and its TypeData.
    void sendViewerEffect(const Uuid& id, uint8_t type, float duration,
                          std::vector<uint8_t> typeData);
    // The effect's 57 TypeData bytes: our id, the target's, the position
    // (three little-endian doubles), the target type.
    static std::vector<uint8_t> packAttention(const Uuid& source, const AttentionEffect& effect);

    // Selecting prims (ObjectSelect) makes their region send their
    // ObjectProperties (name, description, touch and sit text, ...);
    // deselect them once those are in.
    void selectObjects(uint64_t regionHandle, std::span<const uint32_t> localIds);
    void deselectObjects(uint64_t regionHandle, std::span<const uint32_t> localIds);

    // Asks the main region to send an avatar's AvatarAppearance (bakes and
    // visual parameters), as the official viewer does for avatars still
    // drawn with default parameters ("avatartexturesrequest").
    void requestAvatarAppearance(const Uuid& avatar);

    // Where the camera is, in the agent's region's coordinates (agentRegion()). Simulators
    // send objects around the camera (within `farDistance`), so this decides
    // what we get to see. Until it's set, the camera is at the agent.
    struct CameraView
    {
        protocol::Vector3 position{};
        protocol::Vector3 atAxis{1, 0, 0};
        protocol::Vector3 leftAxis{0, 1, 0};
        protocol::Vector3 upAxis{0, 0, 1};
        float farDistance = 128.0f;
    };
    void setCamera(const CameraView& camera) { camera_ = camera; }

    const std::string& loginUri() const { return parameters_.loginUri; }
    // Set once the login server accepted us.
    const LoginSuccess* login() const { return login_ ? &*login_ : nullptr; }
    // Set when state() is Failed.
    const LoginFailure* failure() const { return failure_ ? &*failure_ : nullptr; }

    const std::map<protocol::CircuitId, std::unique_ptr<Region>>& regions() const
    {
        return regions_;
    }
    const Region* mainRegion() const;
    const Region* regionByHandle(uint64_t handle) const;
    // The main region's capability URL, if it granted it. Until a new main
    // region's capabilities arrive (after a teleport), another region's.
    std::optional<std::string_view> capability(std::string_view name) const;

    std::optional<protocol::CircuitStats> circuitStats(protocol::CircuitId circuit) const
    {
        return circuits_.stats(circuit);
    }
    // How many of each message / event arrived (for the debug UI).
    const std::map<std::string, uint64_t, std::less<>>& messageCounts() const
    {
        return messageCounts_;
    }
    const std::map<std::string, uint64_t, std::less<>>& eventCounts() const { return eventCounts_; }

private:
    enum class RequestKind : uint8_t
    {
        Login,
        Seed,
        EventQueue,
    };
    struct Completed
    {
        RequestKind kind;
        protocol::CircuitId circuit; // 0 for the login request
        uint32_t generation;         // the region's httpGeneration when sent
        net::HttpResponse response;
    };

    template <protocol::Message M>
    void send(const Region& region, const M& message, bool reliable = false)
    {
        circuits_.send(region.circuit, message, reliable);
    }

    void sendHttp(RequestKind kind, Region* region, net::HttpRequest request);
    void handleLogin(const net::HttpResponse& response);
    Region* addRegion(const net::Endpoint& endpoint, uint64_t handle, bool isMain);
    void removeRegion(protocol::CircuitId circuit, std::string_view why);
    Region* findRegion(protocol::CircuitId circuit);
    Region* findRegion(const net::Endpoint& endpoint);

    void requestSeed(Region& region);
    // Uses this seed capability from now on (a new one replaces the
    // region's capabilities and event queue).
    void setSeed(Region& region, const std::string& seed);
    void handleSeed(Region& region, const net::HttpResponse& response);
    void pollEventQueue(Region& region);
    void handleEventQueue(Region& region, const net::HttpResponse& response);
    void handleEvent(Region& region, const std::string& name, const llsd::Value& body);

    // TeleportFinish and CrossedRegion: the agent is moving to this
    // simulator, which becomes the main region.
    void moveToRegion(const net::Endpoint& endpoint, uint64_t handle, const std::string& seed,
                      bool teleport);
    void handleAgentMovementComplete(Region& region, const protocol::CircuitEvent& event);
    void handleMapBlockReply(const protocol::CircuitEvent& event);
    void sendTeleportRequest(uint64_t regionHandle, protocol::Vector3 position);
    void teleportFailed(std::string reason);
    // Teleporting or crossing: the main region may be the one we're
    // leaving or one we haven't arrived in yet, so losing it isn't fatal.
    bool movingBetweenRegions() const
    {
        return pendingCrossing_ || teleport_.state == TeleportState::InProgress ||
               teleport_.state == TeleportState::Arriving;
    }

    void handleCircuitEvent(const protocol::CircuitEvent& event);
    void handleRegionHandshake(Region& region, const protocol::CircuitEvent& event);
    void sendHandshakeReply(Region& region, bool haveObjectCache);
    void sendAgentSetup(const Region& region);
    void sendThrottle(const Region& region);
    // Every 5 s: eases or tightens the throttle by the main circuit's loss.
    void updateThrottle(std::chrono::steady_clock::time_point now);
    void sendAgentUpdate(const Region& region);
    void checkConnected();

    void fail(std::string reason, std::string message);
    void finishLogout(std::string_view why);

    LoginParameters parameters_;
    SessionState state_ = SessionState::LoggingIn;
    std::optional<LoginSuccess> login_;
    std::optional<LoginFailure> failure_;

    std::chrono::steady_clock::time_point logoutDeadline_{};
    std::chrono::steady_clock::time_point lastAgentUpdate_{};
    std::map<std::string, uint64_t, std::less<>> messageCounts_;
    std::map<std::string, uint64_t, std::less<>> eventCounts_;
    std::vector<RegionMessage> regionMessages_;
    std::vector<RegionEvent> regionEvents_;
    std::optional<uint64_t> agentRegion_;
    protocol::Vector3 agentPosition_{};
    uint64_t arrivals_ = 0;
    Arrival lastArrival_{};
    std::optional<int32_t> balance_;
    std::optional<CameraView> camera_;

    TeleportStatus teleport_;
    std::chrono::steady_clock::time_point teleportDeadline_{};
    std::optional<Location> lookup_; // the teleport waiting for MapBlockReply
    bool pendingCrossing_ = false;   // CrossedRegion, until AgentMovementComplete

    uint32_t controlFlags_ = 0;
    protocol::Quaternion bodyRotation_{};
    DynamicThrottle throttle_;
    std::chrono::steady_clock::time_point nextThrottleCheck_{};
    protocol::CircuitId throttleCircuit_ = 0; // the main circuit the counts below are from
    uint64_t throttlePacketsIn_ = 0;
    uint64_t throttlePacketsLost_ = 0;
    bool controlsChanged_ = false;
    bool deferHandshakeReplies_ = false;

    // Declaration order is destruction order in reverse: the HTTP groups go
    // first (no more callbacks), then the circuits (network thread joined),
    // and the queues they feed last.
    MessageQueue<Completed> completed_;
    std::vector<Completed> drainedHttp_;
    std::vector<protocol::CircuitEvent> drainedCircuit_;
    protocol::CircuitManager circuits_;
    std::map<protocol::CircuitId, std::unique_ptr<Region>> regions_;
    net::HttpRequestGroup loginHttp_;
};

} // namespace viewer::session
