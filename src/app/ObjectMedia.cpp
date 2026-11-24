#include "ObjectMedia.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <charconv>
#include <format>

namespace viewer
{

namespace
{

constexpr size_t kMaxInFlight = 4;
constexpr uint8_t kMaxAttempts = 3;
// After a failure (or 503: asked too fast): LLMediaDataClient's
// UNAVAILABLE_RETRY_TIMER_DELAY.
constexpr auto kRetryDelay = std::chrono::seconds(5);
constexpr auto kSweepInterval = std::chrono::seconds(2);

} // namespace

MediaEntry parseMediaEntry(const llsd::Value& v)
{
    MediaEntry e;
    e.homeUrl = v["home_url"].asString();
    e.currentUrl = v["current_url"].asString();
    e.autoPlay = v["auto_play"].asBoolean();
    e.autoLoop = v["auto_loop"].asBoolean();
    e.autoScale = v["auto_scale"].asBoolean();
    e.autoZoom = v["auto_zoom"].asBoolean();
    e.firstClickInteract = v["first_click_interact"].asBoolean();
    e.altImageEnable = v["alt_image_enable"].asBoolean();
    e.width = v["width_pixels"].asInteger();
    e.height = v["height_pixels"].asInteger();
    e.controls = v["controls"].asInteger();
    e.whitelistEnable = v["whitelist_enable"].asBoolean();
    const llsd::Value& list = v["whitelist"];
    for(size_t i = 0; i < list.size(); i++)
    {
        e.whitelist.push_back(list[i].asString());
    }
    if(v.has("perms_interact"))
    {
        e.permsInteract = v["perms_interact"].asInteger();
    }
    if(v.has("perms_control"))
    {
        e.permsControl = v["perms_control"].asInteger();
    }
    return e;
}

size_t ObjectMediaData::count() const
{
    size_t n = 0;
    for(const auto& f : faces)
    {
        n += f.has_value();
    }
    return n;
}

std::optional<ObjectMediaData> parseObjectMediaResponse(const llsd::Value& body)
{
    if(!body.isMap() || body.has("error") || !body.has("object_id"))
    {
        return std::nullopt;
    }
    ObjectMediaData d;
    d.object = body["object_id"].asUuid();
    d.version = mediaVersionNumber(body["object_media_version"].asString()).value_or(0);
    const llsd::Value& faces = body["object_media_data"];
    for(size_t i = 0; i < faces.size(); i++)
    {
        const llsd::Value& f = faces[i];
        if(f.isMap())
        {
            d.faces.push_back(parseMediaEntry(f));
        }
        else
        {
            d.faces.push_back(std::nullopt);
        }
    }
    return d;
}

std::optional<uint32_t> mediaVersionNumber(std::string_view version)
{
    constexpr std::string_view kPrefix = "x-mv:";
    const size_t at = version.find(kPrefix);
    if(at == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string_view digits = version.substr(at + kPrefix.size());
    digits = digits.substr(0, digits.find('/'));
    uint32_t n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if(ec != std::errc() || end == digits.data())
    {
        return std::nullopt;
    }
    return n;
}

ObjectMedia::ObjectMedia(net::HttpClient& http)
    : http_(http), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void ObjectMedia::clear()
{
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    std::vector<Answer> stale;
    answers_.drain(stale);
    known_.clear();
    asked_.clear();
    queue_.clear();
    inFlight_ = 0;
    revision_++;
}

const ObjectMediaData* ObjectMedia::find(const Uuid& object) const
{
    const auto it = known_.find(object);
    return it == known_.end() ? nullptr : &it->second;
}

void ObjectMedia::update(
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& withMedia,
    const world::World& world, const Capability& capability)
{
    const auto now = std::chrono::steady_clock::now();

    // Answers.
    answers_.drain(drained_);
    for(Answer& a : drained_)
    {
        inFlight_--;
        const auto asked = asked_.find(a.asked.object);
        if(asked == asked_.end() || asked->second != a.asked.version)
        {
            continue; // gone, or changed again since: asked anew
        }
        if(!a.response.ok())
        {
            if(a.asked.attempts < kMaxAttempts)
            {
                a.asked.notBefore = now + kRetryDelay;
                queue_.push_back(std::move(a.asked));
            }
            else
            {
                log::warn("object media for {}: {}", a.asked.object.toString(),
                          a.response.error != net::HttpError::None
                              ? std::string(net::errorName(a.response.error))
                              : std::format("HTTP {}", a.response.status));
            }
            continue;
        }
        const auto body = llsd::parse(a.response.bodyText());
        std::optional<ObjectMediaData> data = body ? parseObjectMediaResponse(*body) : std::nullopt;
        if(!data || data->object != a.asked.object || data->count() == 0)
        {
            if(known_.erase(a.asked.object) > 0)
            {
                revision_++;
            }
            continue;
        }
        data->key = a.asked.key;
        known_[a.asked.object] = std::move(*data);
        revision_++;
    }

    // New versions to ask for.
    for(const world::ObjectKey& key : withMedia)
    {
        const world::Object* o = world.object(key.region, key.localId);
        if(!o || o->id.isNull())
        {
            continue;
        }
        std::string& asked = asked_[o->id];
        if(asked != o->mediaVersion)
        {
            asked = o->mediaVersion;
            queue_.push_back(Want{key, o->mediaVersion, o->id, 0, {}});
        }
    }

    // The gone (or without media now) are forgotten.
    if(now >= nextSweep_)
    {
        nextSweep_ = now + kSweepInterval;
        std::unordered_set<Uuid> present;
        for(const world::ObjectKey& key : withMedia)
        {
            if(const world::Object* o = world.object(key.region, key.localId))
            {
                present.insert(o->id);
            }
        }
        std::erase_if(asked_, [&](const auto& entry) { return !present.contains(entry.first); });
        if(std::erase_if(known_,
                         [&](const auto& entry) { return !present.contains(entry.first); }) > 0)
        {
            revision_++;
        }
    }

    // Asks, a few at a time.
    for(size_t n = queue_.size(); n > 0 && inFlight_ < kMaxInFlight; n--)
    {
        Want want = std::move(queue_.front());
        queue_.pop_front();
        const auto asked = asked_.find(want.object);
        if(asked == asked_.end() || asked->second != want.version)
        {
            continue; // stale
        }
        const std::string url = capability(want.key.region);
        if(want.notBefore > now || url.empty())
        {
            queue_.push_back(std::move(want)); // later
            continue;
        }
        llsd::Value request;
        request["verb"] = "GET";
        request["object_id"] = want.object;
        const std::string xml = llsd::toXml(request);
        net::HttpRequest r{.method = net::HttpMethod::Post, .url = url};
        r.headers.add("Content-Type", std::string(llsd::kXmlContentType));
        r.headers.add("Accept", std::string(llsd::kXmlContentType));
        r.body.assign(xml.begin(), xml.end());
        want.attempts++;
        inFlight_++;
        group_->send(std::move(r), [this, want](net::HttpResponse&& response)
                     { answers_.push({want, std::move(response)}); });
    }
}

} // namespace viewer
