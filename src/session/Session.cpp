#include "viewer/session/Session.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/protocol/Messages.hpp"

#include <bit>
#include <format>

namespace viewer::session
{

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace msg = protocol::msg;

namespace
{

constexpr int kMaxSeedAttempts = 3;
constexpr auto kLoginTimeout = 60s;
// The simulator holds an event queue request open for about 30 s.
constexpr auto kEventQueueTimeout = 60s;
constexpr auto kLogoutTimeout = 5s;
constexpr auto kAgentUpdateInterval = 1s;

net::HttpRequest llsdPost(std::string url, const llsd::Value& body)
{
    net::HttpRequest request{.method = net::HttpMethod::Post, .url = std::move(url)};
    request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    const std::string xml = llsd::toXml(body);
    request.body.assign(xml.begin(), xml.end());
    return request;
}

// Describes why a response is unusable, or returns nothing if it's fine.
std::optional<std::string> httpProblem(const net::HttpResponse& response)
{
    if(response.error != net::HttpError::None)
    {
        return std::format("{} ({})", net::errorName(response.error), response.errorMessage);
    }
    if(!response.ok())
    {
        return std::format("HTTP status {}", response.status);
    }
    return std::nullopt;
}

// LLSD carries U64s as 8 big-endian bytes.
uint64_t u64FromBinary(const llsd::Binary& bytes)
{
    uint64_t v = 0;
    for(size_t i = 0; i < 8 && i < bytes.size(); i++)
    {
        v = v << 8 | bytes[i];
    }
    return v;
}

// LLSD carries IPv4 addresses as the 4 address bytes in order.
uint32_t ipFromBinary(const llsd::Binary& bytes)
{
    uint32_t v = 0;
    for(size_t i = 0; i < 4 && i < bytes.size(); i++)
    {
        v = v << 8 | bytes[i];
    }
    return v;
}

std::string messageName(protocol::MessageId id)
{
    if(const auto* info = protocol::findMessage(id))
    {
        return std::string(info->name);
    }
    return std::format("unknown 0x{:08X}", id);
}

} // namespace

std::string_view stateName(SessionState state)
{
    switch(state)
    {
    case SessionState::LoggingIn:
        return "logging in";
    case SessionState::Connecting:
        return "connecting to the region";
    case SessionState::Connected:
        return "connected";
    case SessionState::LoggingOut:
        return "logging out";
    case SessionState::LoggedOut:
        return "logged out";
    case SessionState::Failed:
        return "failed";
    }
    return "unknown";
}

std::vector<std::string> defaultCapabilityNames()
{
    return {
        "AgentPreferences", "ChatSessionRequest",   "EventQueueGet",
        "ExtEnvironment",   "FetchInventory2",      "FetchInventoryDescendents2",
        "FetchLib2",        "FetchLibDescendents2", "GetDisplayNames",
        "HomeLocation",     "ObjectMedia",          "ProvisionVoiceAccountRequest",
        "ReadOfflineMsgs",  "SimulatorFeatures",    "UpdateAgentLanguage",
        "ViewerAsset",
    };
}

Session::Session(net::HttpClient& http, LoginParameters parameters)
    : parameters_(std::move(parameters)), loginHttp_(http)
{
    log::info("logging in as {} {} at {}", parameters_.name.first, parameters_.name.last,
              parameters_.loginUri);
    net::HttpRequest request = llsdPost(parameters_.loginUri, buildLoginRequest(parameters_));
    request.timeout = kLoginTimeout;
    sendHttp(RequestKind::Login, nullptr, std::move(request));
}

Session::~Session()
{
    // Cancel all HTTP first so no callback runs while members are destroyed.
    loginHttp_.cancelAll();
    regions_.clear();
}

const Region* Session::mainRegion() const
{
    for(const auto& [id, region] : regions_)
    {
        if(region->isMain)
        {
            return region.get();
        }
    }
    return nullptr;
}

std::optional<std::string_view> Session::capability(std::string_view name) const
{
    const Region* region = mainRegion();
    if(!region)
    {
        return std::nullopt;
    }
    const auto it = region->capabilities.find(name);
    if(it == region->capabilities.end())
    {
        return std::nullopt;
    }
    return it->second;
}

Region* Session::findRegion(protocol::CircuitId circuit)
{
    const auto it = regions_.find(circuit);
    return it == regions_.end() ? nullptr : it->second.get();
}

Region* Session::findRegion(const net::Endpoint& endpoint)
{
    for(auto& [id, region] : regions_)
    {
        if(region->endpoint == endpoint)
        {
            return region.get();
        }
    }
    return nullptr;
}

void Session::sendHttp(RequestKind kind, Region* region, net::HttpRequest request)
{
    const protocol::CircuitId circuit = region ? region->circuit : 0;
    net::HttpRequestGroup& group = region ? *region->http : loginHttp_;
    group.send(std::move(request), [this, kind, circuit](net::HttpResponse&& response)
               { completed_.push({kind, circuit, std::move(response)}); });
}

// --- update -----------------------------------------------------------------

void Session::update()
{
    if(state_ == SessionState::Failed || state_ == SessionState::LoggedOut)
    {
        return;
    }
    const auto now = Clock::now();

    completed_.drain(drainedHttp_);
    for(const Completed& c : drainedHttp_)
    {
        if(c.kind == RequestKind::Login)
        {
            handleLogin(c.response);
        }
        else if(Region* region = findRegion(c.circuit))
        {
            if(c.kind == RequestKind::Seed)
            {
                handleSeed(*region, c.response);
            }
            else
            {
                handleEventQueue(*region, c.response);
            }
        }
        // else: the region was removed while the request was in flight.
        if(state_ == SessionState::Failed || state_ == SessionState::LoggedOut)
        {
            return;
        }
    }

    circuits_.drain(drainedCircuit_);
    for(const auto& event : drainedCircuit_)
    {
        handleCircuitEvent(event);
        if(state_ == SessionState::Failed || state_ == SessionState::LoggedOut)
        {
            return;
        }
    }

    for(auto& [id, region] : regions_)
    {
        if(region->seedRetryAt && now >= *region->seedRetryAt)
        {
            region->seedRetryAt.reset();
            requestSeed(*region);
        }
        if(region->eventQueueRetryAt && now >= *region->eventQueueRetryAt)
        {
            region->eventQueueRetryAt.reset();
            pollEventQueue(*region);
        }
    }

    if(state_ == SessionState::Connected && now - lastAgentUpdate_ >= kAgentUpdateInterval)
    {
        if(const Region* main = mainRegion())
        {
            sendAgentUpdate(*main);
        }
    }

    if(state_ == SessionState::LoggingOut && now >= logoutDeadline_)
    {
        finishLogout("no LogoutReply; stopped waiting");
    }
}

// --- login ------------------------------------------------------------------

void Session::handleLogin(const net::HttpResponse& response)
{
    if(state_ != SessionState::LoggingIn)
    {
        return;
    }
    if(auto problem = httpProblem(response))
    {
        fail("http", std::format("Couldn't reach the login server: {}.", *problem));
        return;
    }
    auto reply = llsd::parse(response.bodyText());
    if(!reply)
    {
        fail("llsd", std::format("The login server's reply couldn't be read: {}", reply.error()));
        return;
    }
    auto result = parseLoginReply(*reply);
    if(!result)
    {
        fail(std::move(result.error().reason), std::move(result.error().message));
        return;
    }
    login_ = std::move(*result);
    log::info("logged in as {} {} ({}); region at {}:{}", login_->name.first, login_->name.last,
              login_->agentId.toString(), login_->simIp, login_->simPort);

    const auto endpoint = net::Endpoint::parse(login_->simIp, login_->simPort);
    if(!endpoint)
    {
        fail("incomplete",
             std::format("The login reply's sim_ip '{}' isn't an IPv4 address.", login_->simIp));
        return;
    }
    state_ = SessionState::Connecting;
    const uint64_t handle = static_cast<uint64_t>(login_->regionX) << 32 | login_->regionY;
    Region* main = addRegion(*endpoint, handle, true);
    if(!main)
    {
        return;
    }
    main->seedCapability = login_->seedCapability;
    requestSeed(*main);
}

// --- regions ----------------------------------------------------------------

Region* Session::addRegion(const net::Endpoint& endpoint, uint64_t handle, bool isMain)
{
    auto circuit = circuits_.open(endpoint);
    if(!circuit)
    {
        if(isMain)
        {
            fail("circuit", std::format("Couldn't open a connection to {}: {}", endpoint.toString(),
                                        circuit.error()));
        }
        else
        {
            log::warn("couldn't open a circuit to {}: {}", endpoint.toString(), circuit.error());
        }
        return nullptr;
    }

    auto region = std::make_unique<Region>();
    region->circuit = *circuit;
    region->endpoint = endpoint;
    region->handle = handle;
    region->isMain = isMain;
    region->http = std::make_unique<net::HttpRequestGroup>(loginHttp_.client());
    Region* r = region.get();
    regions_.emplace(*circuit, std::move(region));
    log::info("circuit {} to {} ({}, grid {}, {})", r->circuit, endpoint.toString(),
              isMain ? "main region" : "neighbour", r->gridX(), r->gridY());

    msg::UseCircuitCode use;
    use.CircuitCode.Code = login_->circuitCode;
    use.CircuitCode.SessionID = login_->sessionId;
    use.CircuitCode.ID = login_->agentId;
    send(*r, use, true);
    if(isMain)
    {
        msg::CompleteAgentMovement complete;
        complete.AgentData.AgentID = login_->agentId;
        complete.AgentData.SessionID = login_->sessionId;
        complete.AgentData.CircuitCode = login_->circuitCode;
        send(*r, complete, true);
    }
    return r;
}

void Session::removeRegion(protocol::CircuitId circuit, std::string_view why)
{
    const auto it = regions_.find(circuit);
    if(it == regions_.end())
    {
        return;
    }
    log::info("closing circuit {} to {} ('{}'): {}", circuit, it->second->endpoint.toString(),
              it->second->name, why);
    regionMessages_.push_back(
        RegionMessage{RegionMessage::Kind::RegionRemoved, it->second->handle, {}});
    circuits_.close(circuit);
    regions_.erase(it); // cancels its HTTP requests
}

// --- capabilities -----------------------------------------------------------

void Session::requestSeed(Region& region)
{
    region.seedAttempts++;
    llsd::Array names;
    for(const auto& name : defaultCapabilityNames())
    {
        names.emplace_back(name);
    }
    sendHttp(RequestKind::Seed, &region,
             llsdPost(region.seedCapability, llsd::Value(std::move(names))));
}

void Session::handleSeed(Region& region, const net::HttpResponse& response)
{
    std::optional<std::string> problem = httpProblem(response);
    std::expected<llsd::Value, std::string> reply;
    if(!problem)
    {
        reply = llsd::parse(response.bodyText());
        if(!reply)
        {
            problem = reply.error();
        }
        else if(!reply->isMap())
        {
            problem = "the reply isn't an LLSD map";
        }
    }

    if(problem)
    {
        if(region.seedAttempts < kMaxSeedAttempts)
        {
            log::warn("seed capability request to {} failed ({}); retrying",
                      region.endpoint.toString(), *problem);
            region.seedRetryAt = Clock::now() + region.seedAttempts * 1s;
            return;
        }
        if(region.isMain)
        {
            fail("seed_capability",
                 std::format("The region didn't hand out its capabilities: {}.", *problem));
        }
        else
        {
            log::warn("neighbour {} didn't hand out its capabilities: {}",
                      region.endpoint.toString(), *problem);
        }
        return;
    }

    for(const auto& [name, url] : *reply->get<llsd::Map>())
    {
        region.capabilities.emplace(name, url.asString());
    }
    region.capabilitiesReceived = true;
    log::info("{}: received {} capabilities", region.endpoint.toString(),
              region.capabilities.size());
    if(region.capabilities.contains("EventQueueGet"))
    {
        region.eventQueueRunning = true;
        pollEventQueue(region);
    }
    checkConnected();
}

// --- event queue ------------------------------------------------------------

void Session::pollEventQueue(Region& region)
{
    if(!region.eventQueueRunning || state_ == SessionState::LoggingOut)
    {
        return;
    }
    llsd::Value body;
    body["ack"] = region.eventQueueAck;
    body["done"] = false;
    const auto url = region.capabilities.find("EventQueueGet");
    net::HttpRequest request = llsdPost(url->second, body);
    request.timeout = kEventQueueTimeout;
    sendHttp(RequestKind::EventQueue, &region, std::move(request));
}

void Session::handleEventQueue(Region& region, const net::HttpResponse& response)
{
    const bool answered = response.error == net::HttpError::None;
    if(answered && response.status == 404)
    {
        // The region closed this queue (we left it).
        log::info("{}: event queue closed", region.endpoint.toString());
        region.eventQueueRunning = false;
        return;
    }
    if(response.error == net::HttpError::Timeout || (answered && response.status >= 500))
    {
        // A long poll that ended without events (the simulator answers 502
        // when its own timeout hits). Ask again right away.
        pollEventQueue(region);
        return;
    }
    if(auto problem = httpProblem(response))
    {
        log::warn("{}: event queue: {}; retrying", region.endpoint.toString(), *problem);
        region.eventQueueRetryAt = Clock::now() + 2s;
        return;
    }

    auto reply = llsd::parse(response.bodyText());
    if(!reply)
    {
        log::warn("{}: unreadable event queue reply: {}", region.endpoint.toString(),
                  reply.error());
        region.eventQueueRetryAt = Clock::now() + 2s;
        return;
    }
    region.eventQueueAck = (*reply)["id"];
    const llsd::Value& events = (*reply)["events"];
    const protocol::CircuitId circuit = region.circuit;
    for(size_t i = 0; i < events.size(); i++)
    {
        handleEvent(region, events[i]["message"].asString(), events[i]["body"]);
        if(state_ == SessionState::Failed || !findRegion(circuit))
        {
            return;
        }
    }
    pollEventQueue(region);
}

void Session::handleEvent(Region& region, const std::string& name, const llsd::Value& body)
{
    eventCounts_[name]++;

    if(name == "EnableSimulator")
    {
        const llsd::Value& info = body["SimulatorInfo"][0];
        const net::Endpoint endpoint{ipFromBinary(info["IP"].asBinary()),
                                     static_cast<uint16_t>(info["Port"].asInteger())};
        if(!findRegion(endpoint))
        {
            addRegion(endpoint, u64FromBinary(info["Handle"].asBinary()), false);
        }
    }
    else if(name == "EstablishAgentCommunication")
    {
        const auto endpoint = net::Endpoint::parse(body["sim-ip-and-port"].asString());
        Region* target = endpoint ? findRegion(*endpoint) : nullptr;
        if(!target)
        {
            log::warn("EstablishAgentCommunication for unknown simulator {}",
                      body["sim-ip-and-port"].asString());
            return;
        }
        if(target->seedCapability.empty())
        {
            target->seedCapability = body["seed-capability"].asString();
            requestSeed(*target);
        }
    }
    else if(name == "TeleportFinish" || name == "CrossedRegion")
    {
        // Moving the agent to another main region isn't implemented yet.
        log::warn("{} from {} isn't handled yet", name, region.endpoint.toString());
    }
}

// --- UDP --------------------------------------------------------------------

void Session::handleCircuitEvent(const protocol::CircuitEvent& event)
{
    Region* region = findRegion(event.circuit);
    if(!region)
    {
        return;
    }

    if(event.kind == protocol::CircuitEvent::Kind::Lost)
    {
        if(region->isMain)
        {
            fail("connection_lost", "The connection to the region was lost.");
        }
        else
        {
            removeRegion(event.circuit, "no traffic");
        }
        return;
    }

    messageCounts_[messageName(event.messageId)]++;
    regionMessages_.push_back(RegionMessage{RegionMessage::Kind::Message, region->handle, event});

    if(event.is<msg::RegionHandshake>())
    {
        handleRegionHandshake(*region, event);
    }
    else if(event.is<msg::AgentMovementComplete>())
    {
        if(auto m = event.decode<msg::AgentMovementComplete>())
        {
            agentRegion_ = region->handle;
            agentPosition_ = m->Data.Position;
            log::info("arrived at ({:.1f}, {:.1f}, {:.1f}); simulator {}",
                      static_cast<double>(m->Data.Position.x),
                      static_cast<double>(m->Data.Position.y),
                      static_cast<double>(m->Data.Position.z),
                      protocol::toString(m->SimData.ChannelVersion));
        }
    }
    else if(event.is<msg::LogoutReply>())
    {
        if(state_ == SessionState::LoggingOut && region->isMain)
        {
            finishLogout("LogoutReply received");
        }
    }
    else if(event.is<msg::DisableSimulator>())
    {
        if(region->isMain)
        {
            fail("disabled", "The region closed the connection.");
        }
        else
        {
            removeRegion(event.circuit, "DisableSimulator");
        }
    }
    else if(event.is<msg::KickUser>())
    {
        const auto kick = event.decode<msg::KickUser>();
        const std::string reason = kick ? protocol::toString(kick->UserInfo.Reason) : "";
        fail("kicked", reason.empty() ? "You were disconnected by the region." : reason);
    }
}

void Session::handleRegionHandshake(Region& region, const protocol::CircuitEvent& event)
{
    const auto handshake = event.decode<msg::RegionHandshake>();
    if(!handshake)
    {
        log::warn("unreadable RegionHandshake from {}", region.endpoint.toString());
        return;
    }
    region.name = protocol::toString(handshake->RegionInfo.SimName);
    region.regionId = handshake->RegionInfo2.RegionID;

    msg::RegionHandshakeReply reply;
    reply.AgentData.AgentID = login_->agentId;
    reply.AgentData.SessionID = login_->sessionId;
    // 0x2: our object cache for this region is empty, so the simulator sends
    // full object updates instead of cache probes. (0x1 would ask for all
    // cacheable objects as probes; 0x4 means we support self-appearance.
    // Checked against llviewerregion.cpp.)
    reply.RegionInfo.Flags = 0x2;
    send(region, reply, true);

    const bool first = !region.handshakeReceived;
    region.handshakeReceived = true;
    log::info("{}: handshake from '{}'", region.endpoint.toString(), region.name);
    if(first && region.isMain)
    {
        sendAgentSetup(region);
    }
    checkConnected();
}

void Session::sendAgentSetup(const Region& region)
{
    // Bandwidth per category in bits per second (same as pymetaverse).
    constexpr float kThrottles[] = {
        150'000, // resend
        170'000, // land
        34'000,  // wind
        34'000,  // cloud
        446'000, // task (objects)
        446'000, // texture
        220'000, // asset
    };
    msg::AgentThrottle throttle;
    throttle.AgentData.AgentID = login_->agentId;
    throttle.AgentData.SessionID = login_->sessionId;
    throttle.AgentData.CircuitCode = login_->circuitCode;
    for(const float value : kThrottles)
    {
        const auto bits = std::bit_cast<uint32_t>(value);
        for(int i = 0; i < 4; i++)
        {
            throttle.Throttle.Throttles.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    }
    send(region, throttle, true);

    msg::AgentHeightWidth size;
    size.AgentData.AgentID = login_->agentId;
    size.AgentData.SessionID = login_->sessionId;
    size.AgentData.CircuitCode = login_->circuitCode;
    size.HeightWidthBlock.Height = 1080;
    size.HeightWidthBlock.Width = 1920;
    send(region, size, true);

    msg::AgentFOV fov;
    fov.AgentData.AgentID = login_->agentId;
    fov.AgentData.SessionID = login_->sessionId;
    fov.AgentData.CircuitCode = login_->circuitCode;
    fov.FOVBlock.VerticalAngle = 1.0472f; // 60 degrees
    send(region, fov, true);

    sendAgentUpdate(region);
}

void Session::sendAgentUpdate(const Region& region)
{
    msg::AgentUpdate update;
    update.AgentData.AgentID = login_->agentId;
    update.AgentData.SessionID = login_->sessionId;
    update.AgentData.CameraAtAxis = {1, 0, 0};
    update.AgentData.CameraLeftAxis = {0, 1, 0};
    update.AgentData.CameraUpAxis = {0, 0, 1};
    update.AgentData.Far = 64;
    send(region, update);
    lastAgentUpdate_ = Clock::now();
}

void Session::checkConnected()
{
    if(state_ != SessionState::Connecting)
    {
        return;
    }
    const Region* main = mainRegion();
    if(main && main->handshakeReceived && main->capabilitiesReceived)
    {
        state_ = SessionState::Connected;
        log::info("connected to '{}'", main->name);
    }
}

// --- leaving ----------------------------------------------------------------

void Session::requestLogout()
{
    switch(state_)
    {
    case SessionState::Connecting:
    case SessionState::Connected:
        if(const Region* main = mainRegion())
        {
            msg::LogoutRequest request;
            request.AgentData.AgentID = login_->agentId;
            request.AgentData.SessionID = login_->sessionId;
            send(*main, request, true);
            state_ = SessionState::LoggingOut;
            logoutDeadline_ = Clock::now() + kLogoutTimeout;
            log::info("logging out");
            return;
        }
        finishLogout("no region to log out from");
        return;
    case SessionState::LoggingIn:
        finishLogout("cancelled");
        return;
    case SessionState::LoggingOut:
    case SessionState::LoggedOut:
    case SessionState::Failed:
        return;
    }
}

void Session::finishLogout(std::string_view why)
{
    log::info("logged out ({})", why);
    state_ = SessionState::LoggedOut;
    loginHttp_.cancelAll();
    for(auto& [id, region] : regions_)
    {
        region->http->cancelAll();
        circuits_.close(id);
    }
}

void Session::fail(std::string reason, std::string message)
{
    log::warn("session failed ({}): {}", reason, message);
    failure_ = LoginFailure{std::move(reason), std::move(message)};
    state_ = SessionState::Failed;
    loginHttp_.cancelAll();
    for(auto& [id, region] : regions_)
    {
        region->http->cancelAll();
        circuits_.close(id);
    }
}

} // namespace viewer::session
