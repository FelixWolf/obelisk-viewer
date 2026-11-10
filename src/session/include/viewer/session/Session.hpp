#pragma once

// Everything that belongs to one login. The App holds a
// std::unique_ptr<Session>: logging in creates one, logging out (or a failed
// login) destroys it, and logging in again creates a fresh one, all without
// restarting the viewer.
//
// Threading: HTTP callbacks run on the HttpClient's I/O thread and only push
// the response into a queue. All state changes happen in update(), on the
// thread that owns the Session (currently the main thread).
//
// Lifetime: destroying a Session cancels its pending HTTP requests (through
// its HttpRequestGroup), so it can be destroyed at any time, even mid-login.
// The HttpClient must outlive it.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/session/Login.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::session
{

enum class SessionState : uint8_t
{
    LoggingIn,
    RequestingCapabilities, // asking the region's seed capability for the others
    LoggedIn,               // login and capabilities done; the UDP circuit comes next
    Failed,
};

std::string_view stateName(SessionState state);

// Capability names requested from the region's seed. The region grants a
// subset; Session::capability() tells you which.
std::vector<std::string> defaultCapabilityNames();

class Session
{
public:
    // Starts logging in immediately.
    Session(net::HttpClient& http, LoginParameters parameters);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Processes finished requests and retries. Call regularly (every frame).
    void update();

    SessionState state() const { return state_; }

    // Set once the login server accepted us (from RequestingCapabilities on).
    const LoginSuccess* login() const { return login_ ? &*login_ : nullptr; }
    // Set when state() is Failed.
    const LoginFailure* failure() const { return failure_ ? &*failure_ : nullptr; }

    // Capability URL by name, if the region granted it.
    std::optional<std::string_view> capability(std::string_view name) const;
    const std::map<std::string, std::string, std::less<>>& capabilities() const
    {
        return capabilities_;
    }

private:
    enum class RequestKind : uint8_t
    {
        Login,
        Seed,
    };
    struct Completed
    {
        RequestKind kind;
        net::HttpResponse response;
    };

    void send(RequestKind kind, net::HttpRequest request);
    void sendSeedRequest();
    void handleLogin(const net::HttpResponse& response);
    void handleSeed(const net::HttpResponse& response);
    void fail(std::string reason, std::string message);

    LoginParameters parameters_;
    SessionState state_ = SessionState::LoggingIn;
    std::optional<LoginSuccess> login_;
    std::optional<LoginFailure> failure_;
    std::map<std::string, std::string, std::less<>> capabilities_;

    int seedAttempts_ = 0;
    std::optional<std::chrono::steady_clock::time_point> seedRetryAt_;

    MessageQueue<Completed> completed_;
    std::vector<Completed> drained_;
    // Declared last so it is destroyed first: once it's gone no callback can
    // touch the members above.
    net::HttpRequestGroup http_;
};

} // namespace viewer::session
