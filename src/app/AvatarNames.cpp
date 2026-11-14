#include "AvatarNames.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <format>

namespace viewer
{

namespace
{

// A request's URL grows to about this long (NAME_URL_SEND_THRESHOLD).
constexpr size_t kUrlLength = 3500;
constexpr auto kKeepFor = std::chrono::minutes(20); // MAX_UNREFRESHED_TIME
constexpr auto kRetryAfter = std::chrono::seconds(5);
constexpr uint8_t kMaxAttempts = 3;

} // namespace

std::string AvatarName::userName() const
{
    if(legacyLastName.empty() || legacyLastName == "Resident")
    {
        return legacyFirstName.empty() ? displayName : legacyFirstName;
    }
    return legacyFirstName + " " + legacyLastName;
}

std::optional<std::pair<Uuid, AvatarName>> parseAvatarName(const llsd::Value& agent)
{
    const Uuid id = agent["id"].asUuid();
    if(id.isNull())
    {
        return std::nullopt;
    }
    AvatarName name;
    name.displayName = agent["display_name"].asString();
    name.legacyFirstName = agent["legacy_first_name"].asString();
    name.legacyLastName = agent["legacy_last_name"].asString();
    name.displayNameDefault = agent["is_display_name_default"].asBoolean();
    if(name.displayName.empty())
    {
        name.displayName = agent["username"].asString();
    }
    return std::pair{id, std::move(name)};
}

AvatarNames::AvatarNames(net::HttpClient& http)
    : http_(http), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void AvatarNames::setCapabilityUrl(std::string url)
{
    if(url == url_)
    {
        return;
    }
    url_ = std::move(url);
    // Requests in flight belong to the old URL: drop them and ask again.
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    requesting_ = false;
    std::vector<Answer> stale;
    answers_.drain(stale);
    for(auto& [id, e] : entries_)
    {
        if(e.state == State::Requested)
        {
            queue(id, e);
        }
    }
}

void AvatarNames::queue(const Uuid& id, Entry& entry)
{
    entry.state = State::Queued;
    queue_.push_back(id);
}

const AvatarName* AvatarNames::get(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    auto [it, inserted] = entries_.try_emplace(id);
    Entry& e = it->second;
    if(inserted)
    {
        queue_.push_back(id);
    }
    else if(e.state == State::Ready && std::chrono::steady_clock::now() - e.receivedAt > kKeepFor)
    {
        e.attempts = 0;
        queue(id, e); // the old name meanwhile
    }
    return e.name ? &*e.name : nullptr;
}

void AvatarNames::update()
{
    const auto now = std::chrono::steady_clock::now();
    answers_.drain(drained_);
    for(Answer& a : drained_)
    {
        requesting_ = false;
        if(a.response.ok())
        {
            const auto body = llsd::parse(std::string_view(
                reinterpret_cast<const char*>(a.response.body.data()), a.response.body.size()));
            if(body)
            {
                const llsd::Value& agents = (*body)["agents"];
                for(size_t i = 0; i < agents.size(); i++)
                {
                    auto parsed = parseAvatarName(agents[i]);
                    if(!parsed)
                    {
                        continue;
                    }
                    const auto it = entries_.find(parsed->first);
                    if(it != entries_.end())
                    {
                        it->second.name = std::move(parsed->second);
                        it->second.state = State::Ready;
                        it->second.receivedAt = now;
                    }
                }
            }
            else
            {
                log::warn("display names: unreadable answer");
            }
        }
        else
        {
            log::warn("display names request failed: {}",
                      a.response.error != net::HttpError::None
                          ? std::string(net::errorName(a.response.error))
                          : std::format("HTTP {}", a.response.status));
            nextRequest_ = now + kRetryAfter;
        }
        // Whatever the answer left out (bad_ids among them): again, a few
        // times, then not.
        for(const Uuid& id : a.asked)
        {
            const auto it = entries_.find(id);
            if(it == entries_.end() || it->second.state != State::Requested)
            {
                continue;
            }
            if(it->second.attempts < kMaxAttempts)
            {
                queue(id, it->second);
            }
            else
            {
                it->second.state = it->second.name ? State::Ready : State::Missing;
                it->second.receivedAt = now;
            }
        }
    }
    drained_.clear();

    if(requesting_ || queue_.empty() || url_.empty() || now < nextRequest_)
    {
        return;
    }
    std::string url = url_;
    std::vector<Uuid> asked;
    while(!queue_.empty() && url.size() < kUrlLength)
    {
        const Uuid id = queue_.back();
        queue_.pop_back();
        const auto it = entries_.find(id);
        if(it == entries_.end() || it->second.state != State::Queued)
        {
            continue;
        }
        it->second.state = State::Requested;
        it->second.attempts++;
        url += asked.empty() ? "?ids=" : "&ids=";
        url += id.toString();
        asked.push_back(id);
    }
    if(asked.empty())
    {
        return;
    }
    net::HttpRequest request{.method = net::HttpMethod::Get, .url = std::move(url)};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    request.timeout = std::chrono::seconds(60);
    requesting_ = true;
    nextRequest_ = now;
    group_->send(std::move(request),
                 [this, asked = std::move(asked)](net::HttpResponse&& response) mutable
                 { answers_.push({std::move(asked), std::move(response)}); });
}

} // namespace viewer
