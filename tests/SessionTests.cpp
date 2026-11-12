// Session against a fake grid: login server, seed capability and event queue
// on the loopback HTTP server, simulators on loopback UDP (POSIX-only for now).
#ifndef _WIN32

#include "FakeSimulator.hpp"
#include "LoopbackHttpServer.hpp"

#include "viewer/llsd/Serialization.hpp"
#include "viewer/session/Session.hpp"

#include <doctest/doctest.h>

#include <atomic>

using namespace viewer::session;
using namespace std::chrono_literals;
namespace llsd = viewer::llsd;
namespace msg = viewer::protocol::msg;
using viewer::test::FakeSimulator;
using viewer::test::LoopbackReply;
using viewer::test::LoopbackRequest;

namespace
{

const viewer::Uuid kAgent = *viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255");
const viewer::Uuid kSession = *viewer::Uuid::parse("11111111-2222-3333-4444-555555555555");

LoginParameters testParameters(const viewer::test::LoopbackHttpServer& server)
{
    LoginParameters p;
    p.loginUri = server.url("/login");
    p.name = {"Test", "Resident"};
    p.passwordHash = hashPassword("secret");
    p.channel = "viewer-tests";
    p.version = "0.0.0";
    p.machine = {.platform = "Lnx",
                 .platformVersion = "0.0.0",
                 .platformString = "test",
                 .mac = "0",
                 .id0 = "0"};
    return p;
}

std::string loginSuccessXml(uint16_t httpPort, uint16_t simPort)
{
    llsd::Value reply;
    reply["login"] = "true";
    reply["first_name"] = "\"Test\"";
    reply["last_name"] = "Resident";
    reply["agent_id"] = kAgent;
    reply["session_id"] = kSession;
    reply["secure_session_id"] = *viewer::Uuid::parse("66666666-7777-8888-9999-aaaaaaaaaaaa");
    reply["circuit_code"] = 1234;
    reply["sim_ip"] = "127.0.0.1";
    reply["sim_port"] = simPort;
    reply["region_x"] = 1000 * 256;
    reply["region_y"] = 1000 * 256;
    reply["seed_capability"] = std::format("http://127.0.0.1:{}/seed", httpPort);
    return llsd::toXml(reply);
}

// Pumps update() until `done` or the timeout.
bool pumpUntil(Session& session, auto done, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline)
    {
        session.update();
        if(done())
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool settled(const Session& s)
{
    return s.state() == SessionState::Connected || s.state() == SessionState::Failed;
}

uint16_t portOf(const std::string& url)
{
    const auto colon = url.rfind(':');
    return static_cast<uint16_t>(std::stoi(url.substr(colon + 1)));
}

msg::RegionHandshake handshake(std::string_view name)
{
    msg::RegionHandshake m;
    m.RegionInfo.SimName = viewer::protocol::fromString(name);
    return m;
}

llsd::Value enableSimulator(const viewer::net::Endpoint& endpoint)
{
    llsd::Value info;
    info["Handle"] = llsd::Binary{0, 0x03, 0xE9, 0x00, 0, 0x03, 0xE8, 0x00}; // (1001, 1000) * 256
    info["IP"] = llsd::Binary{
        static_cast<uint8_t>(endpoint.address >> 24), static_cast<uint8_t>(endpoint.address >> 16),
        static_cast<uint8_t>(endpoint.address >> 8), static_cast<uint8_t>(endpoint.address)};
    info["Port"] = endpoint.port;
    llsd::Value event;
    event["message"] = "EnableSimulator";
    event["body"]["SimulatorInfo"].append(info);
    return event;
}

llsd::Binary ipBinary(const viewer::net::Endpoint& endpoint)
{
    return llsd::Binary{
        static_cast<uint8_t>(endpoint.address >> 24), static_cast<uint8_t>(endpoint.address >> 16),
        static_cast<uint8_t>(endpoint.address >> 8), static_cast<uint8_t>(endpoint.address)};
}

llsd::Binary handleBinary(uint64_t handle)
{
    llsd::Binary bytes(8);
    for(int i = 0; i < 8; i++)
    {
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(handle >> (56 - 8 * i));
    }
    return bytes;
}

constexpr uint64_t gridHandle(uint32_t x, uint32_t y)
{
    return static_cast<uint64_t>(x * 256) << 32 | y * 256;
}

msg::AgentMovementComplete movementComplete(float x, float y, float z)
{
    msg::AgentMovementComplete m;
    m.AgentData.AgentID = kAgent;
    m.AgentData.SessionID = kSession;
    m.Data.Position = {x, y, z};
    m.Data.LookAt = {0, 1, 0};
    return m;
}

} // namespace

TEST_CASE("Session: login, circuit, handshake, neighbours and logout")
{
    FakeSimulator sim;
    FakeSimulator neighbour;
    std::atomic<uint16_t> httpPort{0};
    std::atomic<int> eventQueuePolls{0};
    std::string seedRequestBody;
    std::mutex mutex;
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                const auto body = llsd::parse(request.body);
                if(!body || (*body)["passwd"].asString() != hashPassword("secret"))
                {
                    return LoopbackReply{.status = 400};
                }
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            if(request.path == "/seed")
            {
                std::lock_guard lock(mutex);
                seedRequestBody = request.body;
                llsd::Value caps;
                caps["EventQueueGet"] = std::format("http://127.0.0.1:{}/eq", httpPort.load());
                caps["SimulatorFeatures"] = llsd::Uri{"http://127.0.0.1/features"};
                return LoopbackReply{.body = llsd::toXml(caps)};
            }
            if(request.path == "/eq")
            {
                if(eventQueuePolls++ == 0)
                {
                    llsd::Value reply;
                    reply["id"] = 1;
                    reply["events"].append(enableSimulator(neighbour.endpoint()));
                    return LoopbackReply{.body = llsd::toXml(reply)};
                }
                // Later polls: nothing new; the simulator's own timeout.
                std::this_thread::sleep_for(100ms);
                return LoopbackReply{.status = 502};
            }
            return std::nullopt;
        });
    httpPort = portOf(server.url(""));

    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    CHECK(session.state() == SessionState::LoggingIn);

    // Login, then the circuit to the main region.
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::CompleteAgentMovement>() > 0; }));
    CHECK(session.state() == SessionState::Connecting);
    const auto use = sim.take<msg::UseCircuitCode>();
    REQUIRE(use.has_value());
    CHECK(use->second.CircuitCode.Code == 1234);
    CHECK(use->second.CircuitCode.ID == kAgent);
    CHECK(use->second.CircuitCode.SessionID == kSession);
    CHECK((use->first.packet.flags & viewer::protocol::PacketFlags::Reliable) != 0);

    // The region says hello; we're in once capabilities have arrived too.
    sim.send(handshake("Test Region"), true);
    REQUIRE(pumpUntil(session, [&] { return settled(session); }));
    REQUIRE(session.state() == SessionState::Connected);
    REQUIRE(session.mainRegion() != nullptr);
    CHECK(session.mainRegion()->name == "Test Region");
    CHECK(session.mainRegion()->gridX() == 1000);
    CHECK(session.capability("SimulatorFeatures") == "http://127.0.0.1/features");
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::RegionHandshakeReply>() > 0; }));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::AgentThrottle>() > 0; }));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::AgentUpdate>() > 0; }));
    {
        std::lock_guard lock(mutex);
        const auto requested = llsd::parse(seedRequestBody);
        REQUIRE(requested.has_value());
        CHECK(requested->size() == defaultCapabilityNames().size());
    }

    // The event queue announced a neighbour: a second circuit opens.
    REQUIRE(pumpUntil(session, [&] { return neighbour.count<msg::UseCircuitCode>() > 0; }));
    CHECK(session.regions().size() == 2);
    CHECK(neighbour.count<msg::CompleteAgentMovement>() == 0); // only for the main region
    neighbour.send(handshake("Neighbour"), true);
    REQUIRE(pumpUntil(session,
                      [&]
                      {
                          for(const auto& [id, region] : session.regions())
                          {
                              if(!region->isMain && region->name == "Neighbour")
                              {
                                  return region->gridX() == 1001;
                              }
                          }
                          return false;
                      }));
    CHECK(session.eventCounts().at("EnableSimulator") == 1);

    // The neighbour goes away.
    neighbour.send(msg::DisableSimulator{});
    REQUIRE(pumpUntil(session, [&] { return session.regions().size() == 1; }));

    // Log out: LogoutRequest, then LogoutReply ends the session.
    session.requestLogout();
    CHECK(session.state() == SessionState::LoggingOut);
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::LogoutRequest>() > 0; }));
    sim.send(msg::LogoutReply{}, true);
    REQUIRE(pumpUntil(session, [&] { return session.state() == SessionState::LoggedOut; }));
}

TEST_CASE("Session: logout gives up waiting for a silent region")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            return LoopbackReply{.body = llsd::toXml(llsd::Value(llsd::Map{}))};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::UseCircuitCode>() > 0; }));
    session.requestLogout();
    REQUIRE(pumpUntil(session, [&] { return session.state() == SessionState::LoggedOut; }, 8s));
}

TEST_CASE("Session: KickUser ends the session with the region's reason")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            return LoopbackReply{.body = llsd::toXml(llsd::Value(llsd::Map{}))};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::UseCircuitCode>() > 0; }));

    msg::KickUser kick;
    kick.UserInfo.Reason = viewer::protocol::fromString("Logged in from another location");
    sim.send(kick, true);
    REQUIRE(pumpUntil(session, [&] { return session.state() == SessionState::Failed; }));
    CHECK(session.failure()->reason == "kicked");
    CHECK(session.failure()->message == "Logged in from another location");
}

TEST_CASE("Session reports the login server's failure reason")
{
    viewer::test::LoopbackHttpServer server(
        [](const LoopbackRequest&) -> std::optional<LoopbackReply>
        {
            llsd::Value reply;
            reply["login"] = "false";
            reply["reason"] = "key";
            reply["message"] = "Wrong password";
            return LoopbackReply{.body = llsd::toXml(reply)};
        });
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return settled(session); }));
    REQUIRE(session.failure() != nullptr);
    CHECK(session.failure()->reason == "key");
    CHECK(session.failure()->message == "Wrong password");
    CHECK(session.login() == nullptr);
}

TEST_CASE("Session fails cleanly on HTTP errors and garbage replies")
{
    viewer::test::LoopbackHttpServer server(
        [](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.status = 503};
            }
            return LoopbackReply{.body = "this is not llsd"};
        });
    auto http = viewer::net::createHttpClient();

    Session unavailable(*http, testParameters(server));
    REQUIRE(pumpUntil(unavailable, [&] { return settled(unavailable); }));
    CHECK(unavailable.failure()->reason == "http");

    LoginParameters garbage = testParameters(server);
    garbage.loginUri = server.url("/garbage");
    Session unreadable(*http, garbage);
    REQUIRE(pumpUntil(unreadable, [&] { return settled(unreadable); }));
    CHECK(unreadable.failure()->reason == "llsd");
}

TEST_CASE("Session retries the seed capability, then gives up")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    std::atomic<int> seedRequests{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            seedRequests++;
            return LoopbackReply{.status = 500};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return settled(session); }, 15s));
    CHECK(session.failure()->reason == "seed_capability");
    CHECK(seedRequests == 3);
    CHECK(session.login() != nullptr); // the login itself had worked
}

TEST_CASE("Destroying a Session mid-login is safe")
{
    viewer::test::LoopbackHttpServer server; // /slow takes up to 3 s
    auto http = viewer::net::createHttpClient();
    for(int i = 0; i < 5; i++)
    {
        LoginParameters p = testParameters(server);
        p.loginUri = server.url("/slow");
        auto session = std::make_unique<Session>(*http, p);
        std::this_thread::sleep_for(20ms);
        session.reset(); // cancels the request; its callback must never run
    }
}

TEST_CASE("Session: teleport by region name, then walk into a neighbour")
{
    FakeSimulator start;       // where we log in, grid (1000, 1000)
    FakeSimulator destination; // "Far Away", grid (2000, 2000)
    FakeSimulator next;        // its eastern neighbour, (2001, 2000)
    std::atomic<uint16_t> httpPort{0};
    std::atomic<bool> finishTeleport{false};
    std::atomic<bool> cross{false};
    std::atomic<int> startPolls{0};
    std::atomic<int> destinationPolls{0};

    auto caps = [&](std::string_view queue)
    {
        llsd::Value c;
        c["EventQueueGet"] = std::format("http://127.0.0.1:{}/{}", httpPort.load(), queue);
        c["ViewerAsset"] = std::format("http://assets/{}", queue);
        return LoopbackReply{.body = llsd::toXml(c)};
    };
    auto events = [](llsd::Value event)
    {
        llsd::Value reply;
        reply["id"] = 1;
        reply["events"].append(std::move(event));
        return LoopbackReply{.body = llsd::toXml(reply)};
    };
    auto idle = []
    {
        std::this_thread::sleep_for(50ms);
        return LoopbackReply{.status = 502};
    };
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, start.endpoint().port)};
            }
            if(request.path == "/seed")
            {
                return caps("eq-start");
            }
            if(request.path == "/seed-destination")
            {
                return caps("eq-destination");
            }
            if(request.path == "/seed-next")
            {
                return caps("eq-next");
            }
            if(request.path == "/eq-start" && finishTeleport && startPolls++ == 0)
            {
                llsd::Value info;
                info["AgentID"] = kAgent;
                info["SimIP"] = ipBinary(destination.endpoint());
                info["SimPort"] = destination.endpoint().port;
                info["RegionHandle"] = handleBinary(gridHandle(2000, 2000));
                info["SeedCapability"] =
                    std::format("http://127.0.0.1:{}/seed-destination", httpPort.load());
                llsd::Value event;
                event["message"] = "TeleportFinish";
                event["body"]["Info"].append(info);
                return events(event);
            }
            if(request.path == "/eq-destination" && cross && destinationPolls++ == 0)
            {
                llsd::Value data;
                data["SimIP"] = ipBinary(next.endpoint());
                data["SimPort"] = next.endpoint().port;
                data["RegionHandle"] = handleBinary(gridHandle(2001, 2000));
                data["SeedCapability"] =
                    std::format("http://127.0.0.1:{}/seed-next", httpPort.load());
                llsd::Value event;
                event["message"] = "CrossedRegion";
                event["body"]["RegionData"].append(data);
                return events(event);
            }
            return idle();
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));

    // Log in and arrive.
    REQUIRE(pumpUntil(session, [&] { return start.count<msg::CompleteAgentMovement>() > 0; }));
    start.send(handshake("Start"), true);
    start.send(movementComplete(10, 20, 30), true);
    REQUIRE(pumpUntil(session, [&] { return settled(session) && session.arrivals() == 1; }));
    REQUIRE(session.state() == SessionState::Connected);
    CHECK(session.lastArrival().kind == Arrival::Kind::Login);
    CHECK(session.agentRegion() == gridHandle(1000, 1000));

    // An unknown name fails with a reason.
    session.teleportTo(Location{.region = "Nowhere"});
    CHECK(session.teleport().state == TeleportState::LookingUp);
    REQUIRE(start.waitFor<msg::MapNameRequest>().has_value());
    start.send(msg::MapBlockReply{}, true);
    REQUIRE(pumpUntil(session, [&] { return session.teleport().state == TeleportState::None; }));
    CHECK(session.teleport().failure.find("Nowhere") != std::string::npos);

    // By name: the map gives the handle, then the start region moves us.
    session.teleportTo(Location{.region = "Far Away", .x = 5, .y = 6, .z = 7});
    const auto lookup = start.waitFor<msg::MapNameRequest>();
    REQUIRE(lookup.has_value());
    CHECK(viewer::protocol::toString(lookup->second.NameData.Name) == "Far Away");
    msg::MapBlockReply blocks;
    blocks.Data.resize(2);
    blocks.Data[0].X = 1999;
    blocks.Data[0].Name = viewer::protocol::fromString("Far Away Too");
    blocks.Data[1].X = 2000;
    blocks.Data[1].Y = 2000;
    blocks.Data[1].Name = viewer::protocol::fromString("far away");
    start.send(blocks, true);
    std::optional<std::pair<FakeSimulator::Received, msg::TeleportLocationRequest>> request;
    REQUIRE(
        pumpUntil(session, [&]
                  { return (request = start.take<msg::TeleportLocationRequest>()).has_value(); }));
    CHECK(request->second.Info.RegionHandle == gridHandle(2000, 2000));
    CHECK(request->second.Info.Position == viewer::protocol::Vector3{5, 6, 7});

    start.send(msg::TeleportStart{}, true);
    REQUIRE(
        pumpUntil(session, [&] { return session.teleport().state == TeleportState::InProgress; }));
    msg::TeleportProgress progress;
    progress.Info.Message = viewer::protocol::fromString("sending_dest");
    start.send(progress, true);
    REQUIRE(pumpUntil(session, [&] { return session.teleport().message == "sending_dest"; }));

    // TeleportFinish: a circuit to the destination, which becomes the main
    // region; until its capabilities arrive the old region's serve.
    finishTeleport = true;
    REQUIRE(
        pumpUntil(session, [&] { return destination.count<msg::CompleteAgentMovement>() > 0; }));
    CHECK(destination.count<msg::UseCircuitCode>() == 1);
    CHECK(session.teleport().state == TeleportState::Arriving);
    REQUIRE(session.mainRegion() != nullptr);
    CHECK(session.mainRegion()->handle == gridHandle(2000, 2000));
    CHECK(session.capability("ViewerAsset").has_value());

    // The old region lets go of us before we've arrived: that's fine.
    start.send(msg::DisableSimulator{});
    REQUIRE(pumpUntil(session, [&] { return session.regions().size() == 1; }));
    CHECK(session.state() == SessionState::Connected);

    destination.send(handshake("Far Away"), true);
    destination.send(movementComplete(5, 6, 7), true);
    REQUIRE(pumpUntil(session, [&] { return session.arrivals() == 2; }));
    CHECK(session.lastArrival().kind == Arrival::Kind::Teleport);
    CHECK(session.agentRegion() == gridHandle(2000, 2000));
    CHECK(session.agentPosition() == viewer::protocol::Vector3{5, 6, 7});
    CHECK(session.teleport().state == TeleportState::None);
    CHECK(session.teleport().failure.empty());
    REQUIRE(pumpUntil(session, [&] { return destination.count<msg::AgentThrottle>() > 0; }));
    REQUIRE(
        pumpUntil(session, [&]
                  { return session.capability("ViewerAsset") == "http://assets/eq-destination"; }));

    // Walking: the controls reach the main region right away.
    session.setControls(control::kAtPos, viewer::protocol::Quaternion{0, 0, 0.7071f, 0.7071f});
    REQUIRE(pumpUntil(
        session,
        [&]
        {
            while(auto update = destination.take<msg::AgentUpdate>())
            {
                if(update->second.AgentData.ControlFlags == control::kAtPos)
                {
                    return true;
                }
            }
            return false;
        },
        1s));

    // CrossedRegion: into the neighbour.
    cross = true;
    REQUIRE(pumpUntil(session, [&] { return next.count<msg::CompleteAgentMovement>() > 0; }));
    CHECK(session.mainRegion()->handle == gridHandle(2001, 2000));
    next.send(handshake("Next"), true);
    next.send(movementComplete(1, 6, 7), true);
    REQUIRE(pumpUntil(session, [&] { return session.arrivals() == 3; }));
    CHECK(session.lastArrival().kind == Arrival::Kind::Crossing);
    CHECK(session.agentRegion() == gridHandle(2001, 2000));
    // The region we left is a neighbour now; losing it is no failure.
    destination.send(msg::DisableSimulator{});
    REQUIRE(pumpUntil(session, [&] { return session.regions().size() == 1; }));
    CHECK(session.state() == SessionState::Connected);
}

TEST_CASE("Session: a refused teleport keeps us where we are")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            return LoopbackReply{.body = llsd::toXml(llsd::Value(llsd::Map{}))};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::CompleteAgentMovement>() > 0; }));
    sim.send(handshake("Here"), true);
    sim.send(movementComplete(1, 2, 3), true);
    REQUIRE(pumpUntil(session, [&] { return settled(session) && session.arrivals() == 1; }));

    session.teleportHome();
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::TeleportLandmarkRequest>() > 0; }));
    CHECK(session.teleport().state == TeleportState::Requested);
    session.teleportHome(); // ignored while one is under way
    msg::TeleportFailed failed;
    failed.Info.AgentID = kAgent;
    failed.Info.Reason = viewer::protocol::fromString("Home is unavailable.");
    sim.send(failed, true);
    REQUIRE(pumpUntil(session, [&] { return session.teleport().state == TeleportState::None; }));
    CHECK(session.teleport().failure == "Home is unavailable.");
    CHECK(sim.count<msg::TeleportLandmarkRequest>() == 1);
    CHECK(session.mainRegion()->name == "Here");
    CHECK(session.state() == SessionState::Connected);
}

TEST_CASE("Session: handshake replies wait for the object cache; misses are requested")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            return LoopbackReply{.body = llsd::toXml(llsd::Value(llsd::Map{}))};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    session.deferHandshakeReplies();
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::CompleteAgentMovement>() > 0; }));
    sim.send(handshake("Cached"), true);
    REQUIRE(pumpUntil(session, [&] { return settled(session); }));

    // Nothing until we say the cache is ready.
    pumpUntil(session, [] { return false; }, 300ms);
    CHECK(sim.count<msg::RegionHandshakeReply>() == 0);
    const uint64_t handle = session.mainRegion()->handle;
    session.replyToHandshake(handle, true);
    std::optional<std::pair<FakeSimulator::Received, msg::RegionHandshakeReply>> reply;
    REQUIRE(pumpUntil(session,
                      [&] { return (reply = sim.take<msg::RegionHandshakeReply>()).has_value(); }));
    CHECK(reply->second.RegionInfo.Flags == 0); // we have a cache: probe us

    // 300 misses: two messages (255 blocks at most each).
    std::vector<std::pair<uint32_t, uint8_t>> misses;
    for(uint32_t i = 0; i < 300; i++)
    {
        misses.emplace_back(i + 1, static_cast<uint8_t>(i % 2));
    }
    session.requestObjects(handle, misses);
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::RequestMultipleObjects>() == 2; }));
    const auto first = sim.take<msg::RequestMultipleObjects>();
    REQUIRE(first.has_value());
    CHECK(first->second.ObjectData.size() == 255);
    CHECK(first->second.ObjectData[1].ID == 2);
    CHECK(first->second.ObjectData[1].CacheMissType == 1);
}

TEST_CASE("Session: avatar appearance requests go to the main region")
{
    FakeSimulator sim;
    std::atomic<uint16_t> httpPort{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(httpPort, sim.endpoint().port)};
            }
            return LoopbackReply{.body = llsd::toXml(llsd::Value(llsd::Map{}))};
        });
    httpPort = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(pumpUntil(session, [&] { return sim.count<msg::CompleteAgentMovement>() > 0; }));

    const auto avatar = viewer::Uuid::parse("6f1cbf7d-7945-47cb-8428-8cc14a60e94c");
    REQUIRE(avatar.has_value());
    session.requestAvatarAppearance(*avatar);
    std::optional<std::pair<FakeSimulator::Received, msg::GenericMessage>> request;
    REQUIRE(pumpUntil(session,
                      [&] { return (request = sim.take<msg::GenericMessage>()).has_value(); }));
    const auto text = [](const std::vector<uint8_t>& bytes)
    { return std::string(bytes.begin(), bytes.end()); };
    CHECK(text(request->second.MethodData.Method) == std::string("avatartexturesrequest\0", 22));
    REQUIRE(request->second.ParamList.size() == 1);
    CHECK(text(request->second.ParamList[0].Parameter) == avatar->toString() + '\0');
    CHECK(request->second.AgentData.AgentID == session.login()->agentId);
}

#endif // _WIN32
