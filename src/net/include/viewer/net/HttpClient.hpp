#pragma once

// HTTP behind an interface, so the library doing the work (currently libcurl)
// can be swapped without touching callers. To add a backend, implement
// HttpClient in a new .cpp under src/net and have it define createHttpClient().
//
// Threading: a client runs its own I/O thread(s). send() and cancel() may be
// called from any thread. Callbacks run on the client's I/O thread, so they
// must be quick: copy what you need into a MessageQueue and return.
//
// Lifetime: after cancel(id) returns, that request's callback is guaranteed
// not to run and not to be running (unless cancel was called from inside that
// very callback). Destroying the client cancels everything the same way. That
// guarantee is what lets a Session be torn down safely mid-request; see
// HttpRequestGroup.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace viewer::net
{

enum class HttpMethod : uint8_t
{
    Get,
    Head,
    Post,
    Put,
    Patch,
    Delete,
    Options,
};

std::string_view methodName(HttpMethod method);

// Ordered header list. Lookups are case-insensitive. Response header names
// are lowercased by the backend.
class HttpHeaders
{
public:
    using Entry = std::pair<std::string, std::string>;

    void add(std::string name, std::string value);
    // Replaces every existing header with this name.
    void set(std::string name, std::string value);
    std::optional<std::string_view> find(std::string_view name) const;
    void clear() { entries_.clear(); }

    size_t size() const { return entries_.size(); }
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }

private:
    std::vector<Entry> entries_;
};

struct HttpRequest
{
    HttpMethod method = HttpMethod::Get;
    std::string url;
    HttpHeaders headers{};
    std::vector<uint8_t> body{};

    // Whole request, including the body transfer; 0 = no limit.
    std::chrono::milliseconds timeout{30'000};
    std::chrono::milliseconds connectTimeout{10'000};
    bool followRedirects = true;
};

enum class HttpError : uint8_t
{
    None, // the server answered (check status)
    Timeout,
    CouldNotResolve, // DNS failure
    CouldNotConnect,
    Tls,            // handshake or certificate verification failed
    InvalidRequest, // e.g. malformed URL or unsupported scheme
    Transport,      // anything else (connection reset, ...)
};

std::string_view errorName(HttpError error);

using HttpRequestId = uint64_t;

struct HttpResponse
{
    HttpRequestId requestId = 0; // the id send() returned for this request
    HttpError error = HttpError::None;
    std::string errorMessage{}; // backend detail when error != None
    int status = 0;             // HTTP status; 0 if no response was received
    HttpHeaders headers{};      // of the final response (after redirects)
    std::vector<uint8_t> body{};

    bool ok() const { return error == HttpError::None && status >= 200 && status < 300; }
    std::string_view bodyText() const
    {
        return {reinterpret_cast<const char*>(body.data()), body.size()};
    }
};

using HttpCallback = std::function<void(HttpResponse&& response)>;

struct HttpClientOptions
{
    std::string userAgent = "viewer";
    // Empty: the TLS backend's default trust store.
    std::string caBundlePath{};
    bool verifyPeer = true;
    long maxConnectionsPerHost = 8;
    long maxTotalConnections = 64;
};

class HttpClient
{
public:
    // Cancels all requests; no callback runs after (or during) destruction.
    virtual ~HttpClient() = default;

    // Starts a request. The callback is called exactly once unless the
    // request is cancelled first.
    virtual HttpRequestId send(HttpRequest request, HttpCallback callback) = 0;

    // Returns true if the request was still pending (its callback will now
    // never run), false if it had already completed or never existed.
    virtual bool cancel(HttpRequestId id) = 0;
};

// Creates the default backend.
std::unique_ptr<HttpClient> createHttpClient(const HttpClientOptions& options = {});

// Tracks the requests sent by one owner (a Session, a capability, ...) and
// cancels whatever is still pending when the group is destroyed. Owners that
// hold a group as a member can capture `this` in callbacks safely, as long as
// the group is destroyed before anything the callbacks touch.
class HttpRequestGroup
{
public:
    explicit HttpRequestGroup(HttpClient& client) : client_(client) {}
    ~HttpRequestGroup() { cancelAll(); }

    HttpRequestGroup(const HttpRequestGroup&) = delete;
    HttpRequestGroup& operator=(const HttpRequestGroup&) = delete;

    HttpRequestId send(HttpRequest request, HttpCallback callback);
    bool cancel(HttpRequestId id);
    void cancelAll();
    size_t pendingCount() const;
    HttpClient& client() const { return client_; }

private:
    HttpClient& client_;
    mutable std::mutex mutex_;
    std::unordered_set<HttpRequestId> pending_;
    // Requests whose callback finished before send() recorded their id.
    std::unordered_set<HttpRequestId> completedEarly_;
};

} // namespace viewer::net
