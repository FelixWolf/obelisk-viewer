// Session against a fake login server and seed capability on the loopback
// HTTP server (POSIX-only for now).
#ifndef _WIN32

#include "LoopbackHttpServer.hpp"

#include "viewer/llsd/Serialization.hpp"
#include "viewer/session/Session.hpp"

#include <doctest/doctest.h>

#include <atomic>

using namespace viewer::session;
using namespace std::chrono_literals;
namespace llsd = viewer::llsd;
using viewer::test::LoopbackReply;
using viewer::test::LoopbackRequest;

namespace
{

LoginParameters testParameters(const viewer::test::LoopbackHttpServer& server)
{
    LoginParameters p;
    p.loginUri = server.url("/login");
    p.name = {"Test", "Resident"};
    p.passwordHash = hashPassword("secret");
    p.channel = "viewer-tests";
    p.version = "0.0.0";
    p.machine = {
        .platform = "Lnx", .platformVersion = "test", .mac = "AA:BB:CC:DD:EE:FF", .id0 = "0"};
    return p;
}

std::string loginSuccessXml(uint16_t port)
{
    llsd::Value reply;
    reply["login"] = "true";
    reply["first_name"] = "\"Test\"";
    reply["last_name"] = "Resident";
    reply["agent_id"] = *viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255");
    reply["session_id"] = *viewer::Uuid::parse("11111111-2222-3333-4444-555555555555");
    reply["secure_session_id"] = *viewer::Uuid::parse("66666666-7777-8888-9999-aaaaaaaaaaaa");
    reply["circuit_code"] = 1234;
    reply["sim_ip"] = "127.0.0.1";
    reply["sim_port"] = 13005;
    reply["seed_capability"] = std::format("http://127.0.0.1:{}/seed", port);
    return llsd::toXml(reply);
}

// Pumps update() until the session leaves the given in-progress states.
SessionState runUntilSettled(Session& session, std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline)
    {
        session.update();
        if(session.state() == SessionState::LoggedIn || session.state() == SessionState::Failed)
        {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    return session.state();
}

uint16_t portOf(const std::string& url)
{
    const auto colon = url.rfind(':');
    return static_cast<uint16_t>(std::stoi(url.substr(colon + 1)));
}

} // namespace

TEST_CASE("Session logs in and fetches capabilities")
{
    std::atomic<uint16_t> port{0};
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
                return LoopbackReply{.body = loginSuccessXml(port)};
            }
            if(request.path == "/seed")
            {
                std::lock_guard lock(mutex);
                seedRequestBody = request.body;
                llsd::Value caps;
                caps["EventQueueGet"] = "http://127.0.0.1/eq";
                caps["SimulatorFeatures"] = llsd::Uri{"http://127.0.0.1/features"};
                return LoopbackReply{.body = llsd::toXml(caps)};
            }
            return std::nullopt;
        });
    port = portOf(server.url(""));

    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    CHECK(session.state() == SessionState::LoggingIn);
    REQUIRE(runUntilSettled(session) == SessionState::LoggedIn);

    REQUIRE(session.login() != nullptr);
    CHECK(session.login()->name.first == "Test");
    CHECK(session.login()->circuitCode == 1234);
    CHECK(session.capability("EventQueueGet") == "http://127.0.0.1/eq");
    CHECK(session.capability("SimulatorFeatures") == "http://127.0.0.1/features");
    CHECK_FALSE(session.capability("ViewerAsset").has_value());
    CHECK(session.failure() == nullptr);

    // The seed request asked for our capability names.
    std::lock_guard lock(mutex);
    const auto requested = llsd::parse(seedRequestBody);
    REQUIRE(requested.has_value());
    CHECK(requested->size() == defaultCapabilityNames().size());
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
    REQUIRE(runUntilSettled(session) == SessionState::Failed);
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
    REQUIRE(runUntilSettled(unavailable) == SessionState::Failed);
    CHECK(unavailable.failure()->reason == "http");

    LoginParameters garbage = testParameters(server);
    garbage.loginUri = server.url("/garbage");
    Session unreadable(*http, garbage);
    REQUIRE(runUntilSettled(unreadable) == SessionState::Failed);
    CHECK(unreadable.failure()->reason == "llsd");
}

TEST_CASE("Session retries the seed capability, then gives up")
{
    std::atomic<uint16_t> port{0};
    std::atomic<int> seedRequests{0};
    viewer::test::LoopbackHttpServer server(
        [&](const LoopbackRequest& request) -> std::optional<LoopbackReply>
        {
            if(request.path == "/login")
            {
                return LoopbackReply{.body = loginSuccessXml(port)};
            }
            seedRequests++;
            return LoopbackReply{.status = 500};
        });
    port = portOf(server.url(""));
    auto http = viewer::net::createHttpClient();
    Session session(*http, testParameters(server));
    REQUIRE(runUntilSettled(session, 15s) == SessionState::Failed);
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
