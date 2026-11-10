// logintest: logs in headless with a credentials file, stays connected for a
// while, prints what happened, then logs out. For testing the session and
// circuit code against a real grid with a test account.
//
// Usage: logintest --login-file PATH [--seconds N] [--start last|home|REGION]

#include "viewer/session/Credentials.hpp"
#include "viewer/session/Session.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <print>
#include <thread>

using namespace viewer;
using namespace viewer::session;
using namespace std::chrono_literals;

namespace
{

int usage()
{
    std::println(stderr,
                 "usage: logintest --login-file PATH [--seconds N] [--start last|home|REGION]");
    return 2;
}

void printSession(const Session& session)
{
    std::println("state: {}", stateName(session.state()));
    for(const auto& [id, region] : session.regions())
    {
        std::println("region {} '{}'{} grid ({}, {}) at {}: handshake {}, {} capabilities, "
                     "event queue {}",
                     id, region->name, region->isMain ? " [main]" : "", region->gridX(),
                     region->gridY(), region->endpoint.toString(),
                     region->handshakeReceived ? "yes" : "no", region->capabilities.size(),
                     region->eventQueueRunning ? "running" : "stopped");
        if(const auto s = session.circuitStats(id))
        {
            std::println("    ping {} ms, in {}, out {}, resent {}, unacked {}, duplicates {}, "
                         "dropped {}, malformed {}",
                         s->roundTrip.count(), s->packetsIn, s->packetsOut, s->resent, s->unacked,
                         s->duplicates, s->reliableDropped, s->malformed);
        }
    }
    std::println("UDP messages received:");
    for(const auto& [name, count] : session.messageCounts())
    {
        std::println("  {:6}  {}", count, name);
    }
    std::println("event queue events received:");
    for(const auto& [name, count] : session.eventCounts())
    {
        std::println("  {:6}  {}", count, name);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string loginFile;
    int seconds = 20;
    std::string start = "last";
    for(int i = 1; i < argc; i++)
    {
        const std::string_view arg = argv[i];
        if(i + 1 >= argc)
        {
            return usage();
        }
        if(arg == "--login-file")
        {
            loginFile = argv[++i];
        }
        else if(arg == "--seconds")
        {
            const std::string_view v = argv[++i];
            if(std::from_chars(v.data(), v.data() + v.size(), seconds).ec != std::errc())
            {
                return usage();
            }
        }
        else if(arg == "--start")
        {
            start = argv[++i];
        }
        else
        {
            return usage();
        }
    }
    if(loginFile.empty())
    {
        return usage();
    }

    auto credentials = readCredentialsFile(loginFile);
    if(!credentials)
    {
        std::println(stderr, "{}", credentials.error());
        return 1;
    }

    LoginParameters parameters;
    parameters.loginUri = credentials->loginUri;
    parameters.name = credentials->name;
    parameters.passwordHash = std::move(credentials->passwordHash);
    parameters.start =
        start == "last" || start == "home" ? start : std::format("uri:{}&128&128&0", start);
    parameters.channel = "viewer";
    parameters.version = VIEWER_VERSION;
    parameters.machine = queryMachineInfo();

    auto http = net::createHttpClient({.userAgent = std::format("viewer/{}", VIEWER_VERSION)});
    Session session(*http, std::move(parameters));

    // Log in and stay for the requested time (or until something ends it).
    SessionState last = session.state();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now() < deadline)
    {
        session.update();
        if(session.state() != last)
        {
            last = session.state();
            std::println("-> {}", stateName(last));
        }
        if(last == SessionState::Failed || last == SessionState::LoggedOut)
        {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    printSession(session);
    if(const auto* failure = session.failure())
    {
        std::println(stderr, "failed ({}): {}", failure->reason, failure->message);
        return 1;
    }

    session.requestLogout();
    while(session.state() == SessionState::LoggingOut)
    {
        session.update();
        std::this_thread::sleep_for(10ms);
    }
    std::println("-> {}", stateName(session.state()));
    return 0;
}
