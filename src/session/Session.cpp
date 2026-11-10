#include "viewer/session/Session.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <format>

namespace viewer::session
{

using namespace std::chrono_literals;

namespace
{

constexpr int kMaxSeedAttempts = 3;
constexpr auto kLoginTimeout = 60s;

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

} // namespace

std::string_view stateName(SessionState state)
{
    switch(state)
    {
    case SessionState::LoggingIn:
        return "logging in";
    case SessionState::RequestingCapabilities:
        return "requesting capabilities";
    case SessionState::LoggedIn:
        return "logged in";
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
    : parameters_(std::move(parameters)), http_(http)
{
    log::info("logging in as {} {} at {}", parameters_.name.first, parameters_.name.last,
              parameters_.loginUri);
    net::HttpRequest request = llsdPost(parameters_.loginUri, buildLoginRequest(parameters_));
    request.timeout = kLoginTimeout;
    send(RequestKind::Login, std::move(request));
}

void Session::send(RequestKind kind, net::HttpRequest request)
{
    http_.send(std::move(request), [this, kind](net::HttpResponse&& response)
               { completed_.push({kind, std::move(response)}); });
}

void Session::update()
{
    if(seedRetryAt_ && std::chrono::steady_clock::now() >= *seedRetryAt_)
    {
        seedRetryAt_.reset();
        sendSeedRequest();
    }

    completed_.drain(drained_);
    for(const Completed& c : drained_)
    {
        if(state_ == SessionState::Failed)
        {
            break;
        }
        switch(c.kind)
        {
        case RequestKind::Login:
            handleLogin(c.response);
            break;
        case RequestKind::Seed:
            handleSeed(c.response);
            break;
        }
    }
    drained_.clear();
}

std::optional<std::string_view> Session::capability(std::string_view name) const
{
    const auto it = capabilities_.find(name);
    if(it == capabilities_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void Session::handleLogin(const net::HttpResponse& response)
{
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
    state_ = SessionState::RequestingCapabilities;
    sendSeedRequest();
}

void Session::sendSeedRequest()
{
    seedAttempts_++;
    llsd::Array names;
    for(const auto& name : defaultCapabilityNames())
    {
        names.emplace_back(name);
    }
    send(RequestKind::Seed, llsdPost(login_->seedCapability, llsd::Value(std::move(names))));
}

void Session::handleSeed(const net::HttpResponse& response)
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
        if(seedAttempts_ < kMaxSeedAttempts)
        {
            log::warn("seed capability request failed ({}); retrying", *problem);
            seedRetryAt_ = std::chrono::steady_clock::now() + seedAttempts_ * 1s;
            return;
        }
        fail("seed_capability",
             std::format("The region didn't hand out its capabilities: {}.", *problem));
        return;
    }

    for(const auto& [name, url] : *reply->get<llsd::Map>())
    {
        capabilities_.emplace(name, url.asString());
    }
    log::info("received {} capabilities", capabilities_.size());
    state_ = SessionState::LoggedIn;
}

void Session::fail(std::string reason, std::string message)
{
    log::warn("login failed ({}): {}", reason, message);
    failure_ = LoginFailure{std::move(reason), std::move(message)};
    state_ = SessionState::Failed;
    seedRetryAt_.reset();
    http_.cancelAll();
}

} // namespace viewer::session
