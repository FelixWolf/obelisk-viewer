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
    // (from AgentMovementComplete; updated by later messages in time).
    std::optional<uint64_t> agentRegion() const { return agentRegion_; }
    protocol::Vector3 agentPosition() const { return agentPosition_; }

    // Where the camera is, in the agent's region's coordinates. Simulators
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
    // The main region's capability URL, if it granted it.
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
    void handleSeed(Region& region, const net::HttpResponse& response);
    void pollEventQueue(Region& region);
    void handleEventQueue(Region& region, const net::HttpResponse& response);
    void handleEvent(Region& region, const std::string& name, const llsd::Value& body);

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
    std::optional<CameraView> camera_;

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
