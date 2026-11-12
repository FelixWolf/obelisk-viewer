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

#include <chrono>
#include <map>
#include <memory>
#include <optional>
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
} // namespace control

// Capability names requested from each region's seed. Regions grant a subset.
std::vector<std::string> defaultCapabilityNames();

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
    std::string name{};
    Uuid regionId{};

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

    // Where the agent is: its region's handle and the position in it
    // (from AgentMovementComplete and TeleportLocal).
    std::optional<uint64_t> agentRegion() const { return agentRegion_; }
    protocol::Vector3 agentPosition() const { return agentPosition_; }

    // Counts arrivals (AgentMovementComplete, TeleportLocal); lastArrival()
    // is the latest. Compare the count to notice new ones.
    uint64_t arrivals() const { return arrivals_; }
    const Arrival& lastArrival() const { return lastArrival_; }

    // Teleports. Ignored unless Connected and no teleport is under way; the
    // outcome shows in teleport() and, on success, arrivals().
    // By region name: the map gives us its handle first.
    void teleportTo(const Location& location);
    // To a region by handle, `position` in its coordinates.
    void teleportTo(uint64_t regionHandle, protocol::Vector3 position);
    void teleportHome();
    const TeleportStatus& teleport() const { return teleport_; }

    // How the agent wants to move: control::* flags and the direction the
    // body faces. Sent to the main region at once when they change, and
    // repeated while moving.
    void setControls(uint32_t flags, protocol::Quaternion bodyRotation);

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

    // Set once the login server accepted us.
    const LoginSuccess* login() const { return login_ ? &*login_ : nullptr; }
    // Set when state() is Failed.
    const LoginFailure* failure() const { return failure_ ? &*failure_ : nullptr; }

    const std::map<protocol::CircuitId, std::unique_ptr<Region>>& regions() const
    {
        return regions_;
    }
    const Region* mainRegion() const;
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
    void sendAgentSetup(const Region& region);
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
    std::optional<uint64_t> agentRegion_;
    protocol::Vector3 agentPosition_{};
    uint64_t arrivals_ = 0;
    Arrival lastArrival_{};
    std::optional<CameraView> camera_;

    TeleportStatus teleport_;
    std::chrono::steady_clock::time_point teleportDeadline_{};
    std::optional<Location> lookup_; // the teleport waiting for MapBlockReply
    bool pendingCrossing_ = false;   // CrossedRegion, until AgentMovementComplete

    uint32_t controlFlags_ = 0;
    protocol::Quaternion bodyRotation_{};
    bool controlsChanged_ = false;

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
