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

#endif // _WIN32
