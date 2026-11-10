// Backend-independent parts of the HTTP layer.

#include "viewer/net/HttpClient.hpp"

#include <algorithm>
#include <cctype>

namespace viewer::net
{

namespace
{

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y)
                      { return std::tolower(x) == std::tolower(y); });
}

} // namespace

std::string_view methodName(HttpMethod method)
{
    switch(method)
    {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Head:
        return "HEAD";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Patch:
        return "PATCH";
    case HttpMethod::Delete:
        return "DELETE";
    case HttpMethod::Options:
        return "OPTIONS";
    }
    return "GET";
}

std::string_view errorName(HttpError error)
{
    switch(error)
    {
    case HttpError::None:
        return "none";
    case HttpError::Timeout:
        return "timeout";
    case HttpError::CouldNotResolve:
        return "could not resolve host";
    case HttpError::CouldNotConnect:
        return "could not connect";
    case HttpError::Tls:
        return "TLS error";
    case HttpError::InvalidRequest:
        return "invalid request";
    case HttpError::Transport:
        return "transport error";
    }
    return "unknown";
}

void HttpHeaders::add(std::string name, std::string value)
{
    entries_.emplace_back(std::move(name), std::move(value));
}

void HttpHeaders::set(std::string name, std::string value)
{
    std::erase_if(entries_, [&](const Entry& e) { return equalsIgnoreCase(e.first, name); });
    add(std::move(name), std::move(value));
}

std::optional<std::string_view> HttpHeaders::find(std::string_view name) const
{
    for(const auto& [key, value] : entries_)
    {
        if(equalsIgnoreCase(key, name))
        {
            return value;
        }
    }
    return std::nullopt;
}

// --- HttpRequestGroup ---------------------------------------------------------------

HttpRequestId HttpRequestGroup::send(HttpRequest request, HttpCallback callback)
{
    // Not holding mutex_ across client_.send(): callbacks lock mutex_ while
    // the client holds its own callback lock, so doing the reverse here could
    // deadlock. Instead, a callback that finishes before the id is recorded
    // leaves a note in completedEarly_.
    const HttpRequestId id =
        client_.send(std::move(request),
                     [this, callback = std::move(callback)](HttpResponse&& response)
                     {
                         const HttpRequestId finished = response.requestId;
                         callback(std::move(response));
                         std::lock_guard lock(mutex_);
                         if(pending_.erase(finished) == 0)
                         {
                             completedEarly_.insert(finished);
                         }
                     });

    std::lock_guard lock(mutex_);
    if(completedEarly_.erase(id) == 0)
    {
        pending_.insert(id);
    }
    return id;
}

bool HttpRequestGroup::cancel(HttpRequestId id)
{
    {
        std::lock_guard lock(mutex_);
        if(pending_.erase(id) == 0)
        {
            return false;
        }
    }
    return client_.cancel(id);
}

void HttpRequestGroup::cancelAll()
{
    std::unordered_set<HttpRequestId> ids;
    {
        std::lock_guard lock(mutex_);
        ids.swap(pending_);
    }
    // cancel() blocks until a running callback has finished, so after this
    // loop no callback from this group can touch the group or its owner.
    for(HttpRequestId id : ids)
    {
        client_.cancel(id);
    }
}

size_t HttpRequestGroup::pendingCount() const
{
    std::lock_guard lock(mutex_);
    return pending_.size();
}

} // namespace viewer::net
