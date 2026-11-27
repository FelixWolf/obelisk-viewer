#include "viewer/session/Session.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/protocol/Messages.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
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
// While the agent moves, and the soonest after a change.
constexpr auto kMovingUpdateInterval = 200ms;
constexpr auto kMinUpdateInterval = 20ms;
constexpr auto kMapLookupTimeout = 15s;
constexpr auto kTeleportTimeout = 90s;
// MapNameRequest flags: the map's region layer (llworldmapmessage.cpp's LAYER_FLAG).
constexpr uint32_t kMapLayerFlag = 2;

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

std::string lowercase(std::string_view s)
{
    std::string out(s);
    for(char& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
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

std::string_view teleportStateName(TeleportState state)
{
    switch(state)
    {
    case TeleportState::None:
        return "none";
    case TeleportState::LookingUp:
        return "looking up the region";
    case TeleportState::Requested:
        return "requested";
    case TeleportState::InProgress:
        return "teleporting";
    case TeleportState::Arriving:
        return "arriving";
    }
    return "unknown";
}

std::vector<std::string> defaultCapabilityNames()
{
    return {
        "AgentPreferences",
        "AgentProfile",
        "ChatSessionRequest",
        "EventQueueGet",
        "ExtEnvironment",
        "FetchInventory2",
        "InventoryAPIv3",
        "FetchInventoryDescendents2",
        "FetchLib2",
        "FetchLibDescendents2",
        "GetDisplayNames",
        "HomeLocation",
        // PBR terrain's material overrides (TerrainOverrides).
        "ModifyRegion",
        // Never used (we don't edit materials): asking for it is how a
        // viewer tells the simulator it handles PBR materials, which then
        // streams their per-face overrides (GenericStreamingMessage).
        "ModifyMaterialParams",
        // Never fetched: asking for it is how a viewer tells the simulator
        // it handles animated objects, which then sends ObjectAnimation.
        "ObjectAnimation",
        "ObjectMedia",
        "ProvisionVoiceAccountRequest",
        "ReadOfflineMsgs",
        // Legacy (normal and specular) materials by id.
        "RenderMaterials",
        "SimulatorFeatures",
        "UpdateAgentLanguage",
        // Asking the simulator to make our look from the Current Outfit
        // Folder (CurrentOutfit, after we change what's worn).
        "UpdateAvatarAppearance",
        "ViewerAsset",
        "VoiceSignalingRequest",
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

const Region* Session::regionByHandle(uint64_t handle) const
{
    for(const auto& [id, region] : regions_)
    {
        if(region->handle == handle)
        {
            return region.get();
        }
    }
    return nullptr;
}

std::optional<std::string_view> Session::capability(std::string_view name) const
{
    if(const Region* main = mainRegion())
    {
        if(const auto it = main->capabilities.find(name); it != main->capabilities.end())
        {
            return it->second;
        }
        if(main->capabilitiesReceived)
        {
            return std::nullopt;
        }
    }
    // The main region's haven't arrived (yet): one we were in before has
    // the same services.
    for(const auto& [id, region] : regions_)
    {
        if(const auto it = region->capabilities.find(name); it != region->capabilities.end())
        {
            return it->second;
        }
    }
    return std::nullopt;
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
    const uint32_t generation = region ? region->httpGeneration : 0;
    net::HttpRequestGroup& group = region ? *region->http : loginHttp_;
    group.send(std::move(request), [this, kind, circuit, generation](net::HttpResponse&& response)
               { completed_.push({kind, circuit, generation, std::move(response)}); });
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
        else if(Region* region = findRegion(c.circuit);
                region && region->httpGeneration == c.generation)
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
        // else: the region was removed (or re-seeded) while the request was
        // in flight.
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
        if(region->handshakeReplyDue && now - *region->handshakeReplyDue > 3s)
        {
            log::warn("{}: no word on its object cache; replying without",
                      region->endpoint.toString());
            sendHandshakeReply(*region, false);
        }
        if(region->eventQueueRetryAt && now >= *region->eventQueueRetryAt)
        {
            region->eventQueueRetryAt.reset();
            pollEventQueue(*region);
        }
    }

    updateThrottle(now);

    // AgentUpdate: at once when the controls change, often while moving,
    // and once a second otherwise. Not while the simulators move us
    // between regions (as llviewermessage.cpp's send_agent_update).
    const bool moving = controlFlags_ & ~(control::kFly);
    const auto interval = controlsChanged_ ? kMinUpdateInterval
                          : moving         ? kMovingUpdateInterval
                                           : kAgentUpdateInterval;
    const bool betweenRegions =
        teleport_.state == TeleportState::InProgress || teleport_.state == TeleportState::Arriving;
    if(state_ == SessionState::Connected && !betweenRegions && now - lastAgentUpdate_ >= interval)
    {
        if(const Region* main = mainRegion())
        {
            sendAgentUpdate(*main);
        }
    }

    if(teleport_.state != TeleportState::None && now >= teleportDeadline_)
    {
        teleportFailed(teleport_.state == TeleportState::LookingUp ? "The map didn't answer."
                                                                   : "The teleport took too long.");
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

void Session::setSeed(Region& region, const std::string& seed)
{
    if(seed.empty() || seed == region.seedCapability)
    {
        return; // the same seed: its capabilities still hold
    }
    // A new set: drop the old capabilities and the requests using them.
    region.http = std::make_unique<net::HttpRequestGroup>(loginHttp_.client());
    region.httpGeneration++;
    region.seedCapability = seed;
    region.capabilities.clear();
    region.capabilitiesReceived = false;
    region.seedAttempts = 0;
    region.seedRetryAt.reset();
    region.eventQueueRunning = false;
    region.eventQueueAck = {};
    region.eventQueueRetryAt.reset();
    requestSeed(region);
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
    request.pool = net::HttpPool::LongPoll;
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
            setSeed(*target, body["seed-capability"].asString());
        }
    }
    else if(name == "TeleportFinish")
    {
        const llsd::Value& info = body["Info"][0];
        if(info["AgentID"].asUuid() != login_->agentId)
        {
            return;
        }
        const net::Endpoint endpoint{ipFromBinary(info["SimIP"].asBinary()),
                                     static_cast<uint16_t>(info["SimPort"].asInteger())};
        moveToRegion(endpoint, u64FromBinary(info["RegionHandle"].asBinary()),
                     info["SeedCapability"].asString(), true);
    }
    else if(name == "CrossedRegion")
    {
        const llsd::Value& data = body["RegionData"][0];
        const net::Endpoint endpoint{ipFromBinary(data["SimIP"].asBinary()),
                                     static_cast<uint16_t>(data["SimPort"].asInteger())};
        moveToRegion(endpoint, u64FromBinary(data["RegionHandle"].asBinary()),
                     data["SeedCapability"].asString(), false);
    }
    else if(name == "TeleportFailed")
    {
        const llsd::Value& alerts = body["AlertInfo"];
        std::string reason = body["Info"][0]["Reason"].asString();
        if(reason.empty() && alerts.size() > 0)
        {
            reason = alerts[0]["Message"].asString();
        }
        teleportFailed(reason);
    }
    else
    {
        // The app's (ParcelProperties, ...).
        regionEvents_.push_back({region.handle, name, body});
    }
}

// --- moving between regions -------------------------------------------------

void Session::moveToRegion(const net::Endpoint& endpoint, uint64_t handle, const std::string& seed,
                           bool teleport)
{
    Region* target = findRegion(endpoint);
    if(target && target->handle != handle)
    {
        // The same simulator now hosts another region: start over with it.
        removeRegion(target->circuit, "it moved to another region");
        target = nullptr;
    }
    if(!target)
    {
        target = addRegion(endpoint, handle, false);
        if(!target)
        {
            if(teleport)
            {
                teleportFailed("Couldn't connect to the destination.");
                return;
            }
            fail("circuit", "Couldn't connect to the region we walked into.");
            return;
        }
    }
    else if(teleport)
    {
        // As the official viewer: introduce ourselves again either way.
        msg::UseCircuitCode use;
        use.CircuitCode.Code = login_->circuitCode;
        use.CircuitCode.SessionID = login_->sessionId;
        use.CircuitCode.ID = login_->agentId;
        send(*target, use, true);
    }

    log::info("{} to {} (grid {}, {})", teleport ? "teleporting" : "crossing", endpoint.toString(),
              target->gridX(), target->gridY());
    for(auto& [id, region] : regions_)
    {
        region->isMain = region.get() == target;
    }
    msg::CompleteAgentMovement complete;
    complete.AgentData.AgentID = login_->agentId;
    complete.AgentData.SessionID = login_->sessionId;
    complete.AgentData.CircuitCode = login_->circuitCode;
    send(*target, complete, true);
    setSeed(*target, seed);
    if(teleport)
    {
        teleport_.state = TeleportState::Arriving;
        teleportDeadline_ = Clock::now() + kTeleportTimeout;
    }
    else
    {
        pendingCrossing_ = true;
    }
}

void Session::handleAgentMovementComplete(Region& region, const protocol::CircuitEvent& event)
{
    const auto m = event.decode<msg::AgentMovementComplete>();
    if(!m || m->AgentData.AgentID != login_->agentId)
    {
        return;
    }
    if(!region.isMain)
    {
        // We arrived somewhere we didn't expect; the simulator decides.
        for(auto& [id, r] : regions_)
        {
            r->isMain = r.get() == &region;
        }
    }

    Arrival arrival;
    arrival.kind = teleport_.state == TeleportState::Arriving ? Arrival::Kind::Teleport
                   : arrivals_ == 0                           ? Arrival::Kind::Login
                                                              : Arrival::Kind::Crossing;
    arrival.region = region.handle;
    arrival.position = m->Data.Position;
    arrival.lookAt = m->Data.LookAt;
    agentRegion_ = region.handle;
    agentPosition_ = m->Data.Position;
    lastArrival_ = arrival;
    arrivals_++;
    pendingCrossing_ = false;
    if(arrival.kind == Arrival::Kind::Teleport)
    {
        teleport_ = {};
    }
    log::info("arrived in {} at ({:.1f}, {:.1f}, {:.1f}); simulator {}",
              region.name.empty() ? region.endpoint.toString() : region.name,
              static_cast<double>(m->Data.Position.x), static_cast<double>(m->Data.Position.y),
              static_cast<double>(m->Data.Position.z),
              protocol::toString(m->SimData.ChannelVersion));
    // Throttles and view, once the region has the agent (as the official
    // viewer, on every AgentMovementComplete): before that it may drop them.
    sendAgentSetup(region);
    if(arrival.kind == Arrival::Kind::Login)
    {
        msg::MoneyBalanceRequest request;
        request.AgentData.AgentID = login_->agentId;
        request.AgentData.SessionID = login_->sessionId;
        send(region, request, true);
    }
}

void Session::teleportTo(const Location& location)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || teleport_.state != TeleportState::None || !main)
    {
        return;
    }
    teleport_ = {.state = TeleportState::LookingUp};
    teleportDeadline_ = Clock::now() + kMapLookupTimeout;
    lookup_ = location;
    log::info("teleport: looking up '{}'", location.region);

    msg::MapNameRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.AgentData.Flags = kMapLayerFlag;
    request.NameData.Name = protocol::fromString(location.region);
    send(*main, request, true);
}

void Session::requestMapRegion(std::string_view name)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || !main || name.empty())
    {
        return;
    }
    msg::MapNameRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.AgentData.Flags = kMapLayerFlag;
    request.NameData.Name = protocol::fromString(name);
    send(*main, request, true);
}

void Session::requestMapBlocks(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || !main)
    {
        return;
    }
    const auto u16 = [](uint32_t v) { return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFF)); };
    msg::MapBlockRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.AgentData.Flags = kMapLayerFlag;
    request.PositionData.MinX = u16(minX);
    request.PositionData.MaxX = u16(maxX);
    request.PositionData.MinY = u16(minY);
    request.PositionData.MaxY = u16(maxY);
    send(*main, request, true);
}

void Session::requestAgentLocations(uint64_t regionHandle)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || !main)
    {
        return;
    }
    msg::MapItemRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.RequestData.ItemType = kMapAgentLocations;
    request.RequestData.RegionHandle = regionHandle;
    send(*main, request, true);
}

void Session::requestGroupNames(std::span<const Uuid> groups)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || !main || groups.empty())
    {
        return;
    }
    msg::UUIDGroupNameRequest request;
    for(const Uuid& group : groups)
    {
        request.UUIDNameBlock.push_back({group});
    }
    send(*main, request, true);
}

void Session::requestEstateCovenant()
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || !main)
    {
        return;
    }
    msg::EstateCovenantRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    send(*main, request, true);
}

void Session::teleportTo(uint64_t regionHandle, protocol::Vector3 position)
{
    if(state_ != SessionState::Connected || teleport_.state != TeleportState::None)
    {
        return;
    }
    teleport_ = {};
    sendTeleportRequest(regionHandle, position);
}

void Session::stopAnimations(std::span<const Uuid> animations)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    for(const Uuid& id : animations)
    {
        msg::AgentAnimation m;
        m.AgentData.AgentID = login_->agentId;
        m.AgentData.SessionID = login_->sessionId;
        m.AnimationList.push_back({id, false});
        m.PhysicalAvatarEventList.emplace_back();
        send(*main, m, true);
    }
}

void Session::resetAnimationState()
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    const Uuid stand = *Uuid::parse("2408fe9e-df1d-1d7d-f4ff-1384fa7b350f");
    // A null stop clears the overrides.
    for(const auto& [id, start] : {std::pair(Uuid(), false), std::pair(stand, true)})
    {
        msg::AgentAnimation m;
        m.AgentData.AgentID = login_->agentId;
        m.AgentData.SessionID = login_->sessionId;
        m.AnimationList.push_back({id, start});
        m.PhysicalAvatarEventList.emplace_back();
        send(*main, m, true);
    }
}

void Session::sendResetSkeleton(bool animations)
{
    if(!login_)
    {
        return;
    }
    // The target (us), and whether animations too.
    std::vector<uint8_t> data(login_->agentId.bytes.begin(), login_->agentId.bytes.end());
    data.push_back(animations ? 1 : 0);
    sendViewerEffect(Uuid::random(), kResetSkeletonEffect, 1.0f, std::move(data));
}

void Session::say(std::string_view text, int32_t channel, uint8_t type)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::ChatFromViewer chat;
    chat.AgentData.AgentID = login_->agentId;
    chat.AgentData.SessionID = login_->sessionId;
    chat.ChatData.Message = protocol::fromString(text);
    chat.ChatData.Type = type;
    chat.ChatData.Channel = channel;
    send(*main, chat, true);
}

void Session::sendInstantMessage(const Uuid& to, const Uuid& session, uint8_t dialog,
                                 std::string_view text, std::span<const uint8_t> bucket)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::ImprovedInstantMessage im;
    im.AgentData.AgentID = login_->agentId;
    im.AgentData.SessionID = login_->sessionId;
    im.MessageBlock.FromGroup = false;
    im.MessageBlock.ToAgentID = to;
    im.MessageBlock.Offline = 0; // online: the grid keeps it if they aren't
    im.MessageBlock.Dialog = dialog;
    im.MessageBlock.ID = session;
    const std::string name = login_->name.last.empty()
                                 ? login_->name.first
                                 : login_->name.first + " " + login_->name.last;
    im.MessageBlock.FromAgentName = protocol::fromString(name);
    im.MessageBlock.Message = protocol::fromString(text);
    if(bucket.empty())
    {
        im.MessageBlock.BinaryBucket = {0}; // empty (EMPTY_BINARY_BUCKET)
    }
    else
    {
        im.MessageBlock.BinaryBucket.assign(bucket.begin(), bucket.end());
    }
    send(*main, im, true);
}

void Session::teleportViaLure(const Uuid& lure, bool godlike)
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || teleport_.state != TeleportState::None || !main)
    {
        return;
    }
    // TELEPORT_FLAGS_VIA_LURE; a summons: VIA_GODLIKE_LURE | DISABLE_CANCEL
    // (llteleportflags.h; the region works them out itself now).
    msg::TeleportLureRequest request;
    request.Info.AgentID = login_->agentId;
    request.Info.SessionID = login_->sessionId;
    request.Info.LureID = lure;
    request.Info.TeleportFlags = godlike ? (1u << 8 | 1u << 11) : 1u << 2;
    send(*main, request, true);
    teleport_ = {.state = TeleportState::Requested};
    teleportDeadline_ = Clock::now() + kTeleportTimeout;
    log::info("teleport: by an offer");
}

void Session::offerTeleport(const Uuid& to, std::string_view message)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::StartLure lure;
    lure.AgentData.AgentID = login_->agentId;
    lure.AgentData.SessionID = login_->sessionId;
    lure.Info.LureType = 0; // the region fills it in
    lure.Info.Message = protocol::fromString(message);
    lure.TargetData.push_back({to});
    send(*main, lure, true);
}

void Session::answerFriendship(const Uuid& transaction, bool accept, const Uuid& callingCards)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    if(accept)
    {
        msg::AcceptFriendship m;
        m.AgentData.AgentID = login_->agentId;
        m.AgentData.SessionID = login_->sessionId;
        m.TransactionBlock.TransactionID = transaction;
        m.FolderData.push_back({callingCards});
        send(*main, m, true);
    }
    else
    {
        msg::DeclineFriendship m;
        m.AgentData.AgentID = login_->agentId;
        m.AgentData.SessionID = login_->sessionId;
        m.TransactionBlock.TransactionID = transaction;
        send(*main, m, true);
    }
}

void Session::endFriendship(const Uuid& other)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::TerminateFriendship m;
    m.AgentData.AgentID = login_->agentId;
    m.AgentData.SessionID = login_->sessionId;
    m.ExBlock.OtherID = other;
    send(*main, m, true);
}

void Session::grantRights(const Uuid& other, int32_t rights)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::GrantUserRights m;
    m.AgentData.AgentID = login_->agentId;
    m.AgentData.SessionID = login_->sessionId;
    m.Rights.push_back({other, rights});
    send(*main, m, true);
}

void Session::moveInventoryItem(const Uuid& item, const Uuid& folder)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::MoveInventoryItem m;
    m.AgentData.AgentID = login_->agentId;
    m.AgentData.SessionID = login_->sessionId;
    m.AgentData.Stamp = false;
    m.InventoryData.push_back({item, folder, protocol::fromString("")});
    send(*main, m, true);
}

void Session::replyToScriptDialog(uint64_t regionHandle, const Uuid& object, int32_t channel,
                                  int32_t button, std::string_view label)
{
    const Region* region = regionByHandle(regionHandle);
    if(!region || !login_)
    {
        log::info("script dialog reply not sent: its region is gone");
        return;
    }
    msg::ScriptDialogReply reply;
    reply.AgentData.AgentID = login_->agentId;
    reply.AgentData.SessionID = login_->sessionId;
    reply.Data.ObjectID = object;
    reply.Data.ChatChannel = channel;
    reply.Data.ButtonIndex = button;
    reply.Data.ButtonLabel = protocol::fromString(label);
    send(*region, reply, true);
}

void Session::answerScriptPermissions(uint64_t regionHandle, const Uuid& task, const Uuid& item,
                                      int32_t granted)
{
    const Region* region = regionByHandle(regionHandle);
    if(!region || !login_)
    {
        log::info("script permissions answer not sent: its region is gone");
        return;
    }
    msg::ScriptAnswerYes answer;
    answer.AgentData.AgentID = login_->agentId;
    answer.AgentData.SessionID = login_->sessionId;
    answer.Data.TaskID = task;
    answer.Data.ItemID = item;
    answer.Data.Questions = granted;
    send(*region, answer, true);
}

void Session::detach(std::span<const uint32_t> localIds)
{
    const Region* main = mainRegion();
    if(!main || !login_ || localIds.empty())
    {
        return;
    }
    msg::ObjectDetach detach;
    detach.AgentData.AgentID = login_->agentId;
    detach.AgentData.SessionID = login_->sessionId;
    for(const uint32_t id : localIds)
    {
        detach.ObjectData.push_back({id});
    }
    send(*main, detach, true);
}

void Session::attachFromInventory(std::span<const InventoryObject> items)
{
    const Region* main = mainRegion();
    if(!main || !login_ || items.empty())
    {
        return;
    }
    // Four to a message, all under one id (as the official viewer).
    constexpr size_t kPerMessage = 4;
    const Uuid compound = Uuid::random();
    for(size_t first = 0; first < items.size(); first += kPerMessage)
    {
        msg::RezMultipleAttachmentsFromInv m;
        m.AgentData.AgentID = login_->agentId;
        m.AgentData.SessionID = login_->sessionId;
        m.HeaderData.CompoundMsgID = compound;
        m.HeaderData.TotalObjects = static_cast<uint8_t>(std::min<size_t>(items.size(), 255));
        m.HeaderData.FirstDetachAll = false;
        for(size_t i = first; i < std::min(items.size(), first + kPerMessage); i++)
        {
            const InventoryObject& item = items[i];
            msg::RezMultipleAttachmentsFromInv::ObjectDataBlock b;
            b.ItemID = item.itemId;
            b.OwnerID = item.ownerId;
            b.AttachmentPt = kAttachmentAdd; // point 0: where it was last
            b.ItemFlags = item.flags;
            b.GroupMask = item.groupMask;
            b.EveryoneMask = item.everyoneMask;
            b.NextOwnerMask = item.nextOwnerMask;
            b.Name = protocol::fromString(item.name);
            b.Description = protocol::fromString(item.description);
            m.ObjectData.push_back(std::move(b));
        }
        send(*main, m, true);
    }
}

std::vector<uint8_t> Session::packAttention(const Uuid& source, const AttentionEffect& effect)
{
    std::vector<uint8_t> data(57, 0);
    std::copy(source.bytes.begin(), source.bytes.end(), data.begin());
    std::copy(effect.target.bytes.begin(), effect.target.bytes.end(), data.begin() + 16);
    for(size_t i = 0; i < 3; i++)
    {
        // Little-endian whatever the host is (as htolememcpy).
        const auto bits = std::bit_cast<uint64_t>(effect.position[i]);
        for(size_t b = 0; b < 8; b++)
        {
            data[32 + i * 8 + b] = static_cast<uint8_t>(bits >> (8 * b));
        }
    }
    data[56] = effect.targetType;
    return data;
}

void Session::sendAttention(const AttentionEffect& effect)
{
    if(login_)
    {
        sendViewerEffect(effect.id, effect.effectType, effect.duration,
                         packAttention(login_->agentId, effect));
    }
}

void Session::sendViewerEffect(const Uuid& id, uint8_t type, float duration,
                               std::vector<uint8_t> typeData)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    msg::ViewerEffect message;
    message.AgentData.AgentID = login_->agentId;
    message.AgentData.SessionID = login_->sessionId;
    msg::ViewerEffect::EffectBlock block;
    block.ID = id;
    block.AgentID = login_->agentId;
    block.Type = type;
    block.Duration = duration;
    block.Color = {255, 255, 255, 255};
    block.TypeData = std::move(typeData);
    message.Effect.push_back(std::move(block));
    send(*main, message, false);
}

void Session::teleportHome()
{
    const Region* main = mainRegion();
    if(state_ != SessionState::Connected || teleport_.state != TeleportState::None || !main)
    {
        return;
    }
    // A null landmark means home (llagent.cpp).
    msg::TeleportLandmarkRequest request;
    request.Info.AgentID = login_->agentId;
    request.Info.SessionID = login_->sessionId;
    send(*main, request, true);
    teleport_ = {.state = TeleportState::Requested};
    teleportDeadline_ = Clock::now() + kTeleportTimeout;
    log::info("teleport: home");
}

void Session::sendTeleportRequest(uint64_t regionHandle, protocol::Vector3 position)
{
    const Region* main = mainRegion();
    if(!main)
    {
        teleportFailed("Not in a region.");
        return;
    }
    msg::TeleportLocationRequest request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.Info.RegionHandle = regionHandle;
    request.Info.Position = position;
    request.Info.LookAt = camera_ ? camera_->atAxis : protocol::Vector3{0, 1, 0};
    send(*main, request, true);
    teleport_.state = TeleportState::Requested;
    teleportDeadline_ = Clock::now() + kTeleportTimeout;
    log::info("teleport: to grid ({}, {}) at ({:.0f}, {:.0f}, {:.0f})",
              static_cast<uint32_t>(regionHandle >> 32) / 256,
              static_cast<uint32_t>(regionHandle) / 256, static_cast<double>(position.x),
              static_cast<double>(position.y), static_cast<double>(position.z));
}

void Session::handleMapBlockReply(const protocol::CircuitEvent& event)
{
    if(teleport_.state != TeleportState::LookingUp || !lookup_)
    {
        return;
    }
    const auto reply = event.decode<msg::MapBlockReply>();
    if(!reply)
    {
        return;
    }
    const std::string wanted = lowercase(lookup_->region);
    for(const auto& block : reply->Data)
    {
        if(lowercase(protocol::toString(block.Name)) == wanted)
        {
            const uint64_t handle =
                static_cast<uint64_t>(block.X) * 256 << 32 | static_cast<uint64_t>(block.Y) * 256;
            const protocol::Vector3 position{lookup_->x, lookup_->y, lookup_->z};
            lookup_.reset();
            sendTeleportRequest(handle, position);
            return;
        }
    }
    const std::string name = lookup_->region;
    teleportFailed(std::format("There's no region named '{}'.", name));
}

void Session::teleportFailed(std::string reason)
{
    if(reason.empty())
    {
        reason = "The teleport failed.";
    }
    log::warn("teleport failed: {}", reason);
    teleport_ = {.failure = std::move(reason)};
    lookup_.reset();
}

void Session::setControls(uint32_t flags, protocol::Quaternion bodyRotation)
{
    if(flags != controlFlags_ || !(bodyRotation == bodyRotation_))
    {
        controlFlags_ = flags;
        bodyRotation_ = bodyRotation;
        controlsChanged_ = true;
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
        if(region->isMain && !movingBetweenRegions())
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
        handleAgentMovementComplete(*region, event);
    }
    else if(event.is<msg::MapBlockReply>())
    {
        handleMapBlockReply(event);
    }
    else if(event.is<msg::TeleportStart>())
    {
        // Also unasked: the simulator may move us (sent home, say).
        if(teleport_.state != TeleportState::Arriving)
        {
            teleport_.state = TeleportState::InProgress;
            teleport_.failure.clear();
            teleportDeadline_ = Clock::now() + kTeleportTimeout;
        }
    }
    else if(event.is<msg::TeleportProgress>())
    {
        if(const auto m = event.decode<msg::TeleportProgress>();
           m && teleport_.state != TeleportState::None)
        {
            teleport_.message = protocol::toString(m->Info.Message);
        }
    }
    else if(event.is<msg::TeleportFailed>())
    {
        if(const auto m = event.decode<msg::TeleportFailed>())
        {
            std::string reason = protocol::toString(m->Info.Reason);
            if(reason.empty() && !m->AlertInfo.empty())
            {
                reason = protocol::toString(m->AlertInfo[0].Message);
            }
            teleportFailed(std::move(reason));
        }
    }
    else if(event.is<msg::TeleportLocal>())
    {
        if(const auto m = event.decode<msg::TeleportLocal>();
           m && m->Info.AgentID == login_->agentId)
        {
            agentPosition_ = m->Info.Position;
            lastArrival_ = Arrival{Arrival::Kind::LocalTeleport, region->handle, m->Info.Position,
                                   m->Info.LookAt};
            arrivals_++;
            teleport_ = {};
            log::info("teleported within the region to ({:.1f}, {:.1f}, {:.1f})",
                      static_cast<double>(m->Info.Position.x),
                      static_cast<double>(m->Info.Position.y),
                      static_cast<double>(m->Info.Position.z));
        }
    }
    else if(event.is<msg::MoneyBalanceReply>())
    {
        if(const auto m = event.decode<msg::MoneyBalanceReply>();
           m && m->MoneyData.AgentID == login_->agentId)
        {
            balance_ = m->MoneyData.MoneyBalance;
        }
    }
    else if(event.is<msg::SimStats>())
    {
        if(const auto m = event.decode<msg::SimStats>())
        {
            Region::Stats stats = region->stats.value_or(Region::Stats{});
            for(const auto& stat : m->Stat)
            {
                switch(stat.StatID)
                {
                case 0:
                    stats.timeDilation = stat.StatValue;
                    break;
                case 1:
                    stats.fps = stat.StatValue;
                    break;
                case 2:
                    stats.physicsFps = stat.StatValue;
                    break;
                default:
                    break;
                }
            }
            region->stats = stats;
        }
    }
    else if(event.is<msg::AvatarSitResponse>())
    {
        // Seated by the region already, unless it wants us to walk there
        // first (the official viewer's autopilot, then AgentSit): we don't
        // walk, but ask to sit all the same (near enough, it seats us).
        if(const auto m = event.decode<msg::AvatarSitResponse>(); m && login_)
        {
            if(m->SitTransform.AutoPilot)
            {
                msg::AgentSit sit;
                sit.AgentData.AgentID = login_->agentId;
                sit.AgentData.SessionID = login_->sessionId;
                send(*region, sit, true);
            }
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
        if(region->isMain && !movingBetweenRegions())
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
    region.flags = handshake->RegionInfo.RegionFlags;
    if(deferHandshakeReplies_)
    {
        region.handshakeReplyDue = Clock::now();
    }
    else
    {
        sendHandshakeReply(region, false);
    }

    region.handshakeReceived = true;
    log::info("{}: handshake from '{}'", region.endpoint.toString(), region.name);
    checkConnected();
}

void Session::sendHandshakeReply(Region& region, bool haveObjectCache)
{
    region.handshakeReplyDue.reset();
    msg::RegionHandshakeReply reply;
    reply.AgentData.AgentID = login_->agentId;
    reply.AgentData.SessionID = login_->sessionId;
    // 0x2: our object cache for this region is empty, so the simulator sends
    // full object updates instead of cache probes; without it, it probes
    // (ObjectUpdateCached). (0x1 would ask for all cacheable objects as
    // probes; 0x4 means we support self-appearance. Checked against
    // llviewerregion.cpp.)
    reply.RegionInfo.Flags = haveObjectCache ? 0x0 : 0x2;
    send(region, reply, true);
}

void Session::replyToHandshake(uint64_t regionHandle, bool haveObjectCache)
{
    for(auto& [id, region] : regions_)
    {
        if(region->handle == regionHandle && region->handshakeReplyDue)
        {
            sendHandshakeReply(*region, haveObjectCache);
        }
    }
}

void Session::requestObjects(uint64_t regionHandle,
                             std::span<const std::pair<uint32_t, uint8_t>> objects)
{
    const Region* target = nullptr;
    for(const auto& [id, region] : regions_)
    {
        target = region->handle == regionHandle ? region.get() : target;
    }
    if(!target || objects.empty())
    {
        return;
    }
    constexpr size_t kPerMessage = 255; // the block's limit
    for(size_t first = 0; first < objects.size(); first += kPerMessage)
    {
        msg::RequestMultipleObjects request;
        request.AgentData.AgentID = login_->agentId;
        request.AgentData.SessionID = login_->sessionId;
        for(size_t i = first; i < objects.size() && i < first + kPerMessage; i++)
        {
            auto& block = request.ObjectData.emplace_back();
            block.CacheMissType = objects[i].second;
            block.ID = objects[i].first;
        }
        send(*target, request, true);
    }
}

namespace
{

template <typename Block>
Block surfaceBlock(const Session::TouchSurface& s)
{
    Block b;
    b.UVCoord = s.uv;
    b.STCoord = s.st;
    b.FaceIndex = s.face;
    b.Position = s.position;
    b.Normal = s.normal;
    b.Binormal = s.binormal;
    return b;
}

} // namespace

void Session::requestSit(uint64_t regionHandle, const Uuid& object, protocol::Vector3 offset)
{
    const Region* target = nullptr;
    for(const auto& [id, region] : regions_)
    {
        target = region->handle == regionHandle ? region.get() : target;
    }
    if(!target || !login_)
    {
        return;
    }
    msg::AgentRequestSit sit;
    sit.AgentData.AgentID = login_->agentId;
    sit.AgentData.SessionID = login_->sessionId;
    sit.TargetObject.TargetID = object;
    sit.TargetObject.Offset = offset;
    send(*target, sit, true);
}

void Session::touchObject(uint64_t regionHandle, uint32_t localId, protocol::Vector3 grabOffset,
                          const TouchSurface& surface)
{
    const Region* target = nullptr;
    for(const auto& [id, region] : regions_)
    {
        target = region->handle == regionHandle ? region.get() : target;
    }
    if(!target || !login_)
    {
        return;
    }
    msg::ObjectGrab grab;
    grab.AgentData.AgentID = login_->agentId;
    grab.AgentData.SessionID = login_->sessionId;
    grab.ObjectData.LocalID = localId;
    grab.ObjectData.GrabOffset = grabOffset;
    grab.SurfaceInfo.push_back(surfaceBlock<msg::ObjectGrab::SurfaceInfoBlock>(surface));
    send(*target, grab, true);
}

void Session::dragObject(uint64_t regionHandle, const Uuid& object, protocol::Vector3 grabOffset,
                         protocol::Vector3 grabPosition, uint32_t sinceLast,
                         const TouchSurface& surface)
{
    const Region* target = regionByHandle(regionHandle);
    if(!target || !login_)
    {
        return;
    }
    msg::ObjectGrabUpdate update;
    update.AgentData.AgentID = login_->agentId;
    update.AgentData.SessionID = login_->sessionId;
    update.ObjectData.ObjectID = object;
    update.ObjectData.GrabOffsetInitial = grabOffset;
    update.ObjectData.GrabPosition = grabPosition;
    update.ObjectData.TimeSinceLast = sinceLast;
    update.SurfaceInfo.push_back(surfaceBlock<msg::ObjectGrabUpdate::SurfaceInfoBlock>(surface));
    send(*target, update); // unreliable, as the official viewer: the next one follows
}

void Session::releaseObject(uint64_t regionHandle, uint32_t localId, const TouchSurface& surface)
{
    const Region* target = nullptr;
    for(const auto& [id, region] : regions_)
    {
        target = region->handle == regionHandle ? region.get() : target;
    }
    if(!target || !login_)
    {
        return;
    }
    msg::ObjectDeGrab release;
    release.AgentData.AgentID = login_->agentId;
    release.AgentData.SessionID = login_->sessionId;
    release.ObjectData.LocalID = localId;
    release.SurfaceInfo.push_back(surfaceBlock<msg::ObjectDeGrab::SurfaceInfoBlock>(surface));
    send(*target, release, true);
}

namespace
{

template <typename M>
M selectionMessage(const LoginSuccess& login, std::span<const uint32_t> localIds)
{
    M m;
    m.AgentData.AgentID = login.agentId;
    m.AgentData.SessionID = login.sessionId;
    for(uint32_t id : localIds)
    {
        m.ObjectData.push_back({id});
    }
    return m;
}

} // namespace

void Session::selectObjects(uint64_t regionHandle, std::span<const uint32_t> localIds)
{
    for(const auto& [id, region] : regions_)
    {
        if(region->handle == regionHandle && login_ && !localIds.empty())
        {
            send(*region, selectionMessage<msg::ObjectSelect>(*login_, localIds), true);
        }
    }
}

void Session::deselectObjects(uint64_t regionHandle, std::span<const uint32_t> localIds)
{
    for(const auto& [id, region] : regions_)
    {
        if(region->handle == regionHandle && login_ && !localIds.empty())
        {
            send(*region, selectionMessage<msg::ObjectDeselect>(*login_, localIds), true);
        }
    }
}

void Session::requestAvatarAppearance(const Uuid& avatar)
{
    const Region* main = mainRegion();
    if(!main || !login_)
    {
        return;
    }
    // Variable strings go with their terminating nul (as the official
    // viewer's addString).
    auto bytes = [](std::string_view text)
    {
        std::vector<uint8_t> out(text.begin(), text.end());
        out.push_back(0);
        return out;
    };
    msg::GenericMessage request;
    request.AgentData.AgentID = login_->agentId;
    request.AgentData.SessionID = login_->sessionId;
    request.MethodData.Method = bytes("avatartexturesrequest");
    request.ParamList.push_back({bytes(avatar.toString())});
    send(*main, request, true);
}

std::array<float, 7> throttleBitsPerSecond(float kbps)
{
    // Resend, land, wind, cloud, task, texture, asset (kbps), each preset
    // adding up to its total.
    constexpr std::array<std::array<float, 7>, 4> kPresets = {{
        {5, 10, 3, 3, 10, 10, 9},
        {30, 40, 9, 9, 86, 86, 40},
        {50, 70, 14, 14, 136, 136, 80},
        {100, 100, 20, 20, 310, 310, 140},
    }};
    auto total = [](const std::array<float, 7>& p)
    {
        float sum = 0.0f;
        for(const float v : p)
        {
            sum += v;
        }
        return sum;
    };
    kbps = std::clamp(kbps, 50.0f, 6000.0f);
    // The presets either side (the last two beyond the last).
    size_t upper = 0;
    while(upper < kPresets.size() && total(kPresets[upper]) <= kbps)
    {
        upper++;
    }
    std::array<float, 7> out = kPresets[0];
    if(upper > 0)
    {
        const size_t hi = std::min(upper, kPresets.size() - 1);
        const auto& a = kPresets[hi - 1];
        const auto& b = kPresets[hi];
        const auto& base = upper == kPresets.size() ? b : a;
        const float t = (kbps - total(base)) / (total(b) - total(a));
        for(size_t i = 0; i < out.size(); i++)
        {
            out[i] = base[i] + (b[i] - a[i]) * t;
        }
    }
    for(float& v : out)
    {
        v *= 1024.0f; // the simulator takes bits, the presets are kilobits
    }
    return out;
}

void DynamicThrottle::setBandwidth(float kbps)
{
    bandwidth_ = kbps;
    fraction_ = kMaxFraction;
}

bool DynamicThrottle::update(uint64_t arrived, uint64_t lost)
{
    const uint64_t total = arrived + lost;
    const float lossPercent =
        total > 0 ? 100.0f * static_cast<float>(lost) / static_cast<float>(total) : 0.0f;
    const float before = fraction_;
    if(lossPercent > kTightenLossPercent)
    {
        if(current() > kMinKbps)
        {
            fraction_ = std::max(kMinFraction, fraction_ - kStep);
        }
    }
    else if(lossPercent <= kEaseLossPercent)
    {
        if(current() < kMaxKbps)
        {
            fraction_ = std::min(kMaxFraction, fraction_ + kStep);
        }
    }
    return fraction_ != before;
}

void Session::setBandwidth(float kbps)
{
    throttle_.setBandwidth(kbps);
    if(const Region* main = mainRegion(); main && state_ == SessionState::Connected)
    {
        sendThrottle(*main);
    }
}

void Session::updateThrottle(Clock::time_point now)
{
    constexpr auto kInterval = std::chrono::seconds(5); // DYNAMIC_UPDATE_DURATION
    if(now < nextThrottleCheck_)
    {
        return;
    }
    nextThrottleCheck_ = now + kInterval;
    const Region* main = mainRegion();
    if(!main || state_ != SessionState::Connected)
    {
        return;
    }
    const auto stats = circuits_.stats(main->circuit);
    if(!stats)
    {
        return;
    }
    if(throttleCircuit_ != main->circuit)
    {
        // A new main region: count from here.
        throttleCircuit_ = main->circuit;
        throttlePacketsIn_ = stats->packetsIn;
        throttlePacketsLost_ = stats->packetsLost;
        return;
    }
    const uint64_t arrived = stats->packetsIn - throttlePacketsIn_;
    const uint64_t lost = stats->packetsLost - throttlePacketsLost_;
    throttlePacketsIn_ = stats->packetsIn;
    throttlePacketsLost_ = stats->packetsLost;
    if(throttle_.update(arrived, lost))
    {
        log::info("throttle: {} of {} packets lost in 5 s", lost, arrived + lost);
        sendThrottle(*main);
    }
}

void Session::sendThrottle(const Region& region)
{
    log::info("throttle: asking {} for {:.0f} kbps",
              region.name.empty() ? region.endpoint.toString() : region.name,
              static_cast<double>(throttle_.current()));
    msg::AgentThrottle throttle;
    throttle.AgentData.AgentID = login_->agentId;
    throttle.AgentData.SessionID = login_->sessionId;
    throttle.AgentData.CircuitCode = login_->circuitCode;
    for(const float value : throttleBitsPerSecond(throttle_.current()))
    {
        const auto bits = std::bit_cast<uint32_t>(value);
        for(int i = 0; i < 4; i++)
        {
            throttle.Throttle.Throttles.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    }
    send(region, throttle, true);
}

void Session::sendAgentSetup(const Region& region)
{
    sendThrottle(region);

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
    CameraView camera;
    camera.position = agentPosition_;
    if(camera_)
    {
        camera = *camera_;
    }
    update.AgentData.CameraCenter = camera.position;
    update.AgentData.CameraAtAxis = camera.atAxis;
    update.AgentData.CameraLeftAxis = camera.leftAxis;
    update.AgentData.CameraUpAxis = camera.upAxis;
    update.AgentData.Far = camera.farDistance;
    update.AgentData.BodyRotation = bodyRotation_;
    update.AgentData.HeadRotation = bodyRotation_;
    update.AgentData.ControlFlags = controlFlags_;
    send(region, update);
    lastAgentUpdate_ = Clock::now();
    controlsChanged_ = false;
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
