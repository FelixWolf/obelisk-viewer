#include "viewer/voice/VoiceClient.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace viewer::voice
{

namespace
{

using namespace std::chrono_literals;

constexpr auto kInterval = 100ms;
constexpr auto kFeaturesRetry = 10s;
// How long the parcel may take to be known, before it's as if it hadn't a
// channel of its own.
constexpr auto kParcelWait = 5s;
constexpr float kSpeaking = 0.01f; // our own voice's least power

} // namespace

VoiceClient::VoiceClient(net::HttpClient& http, audio::AudioEngine* engine)
    : VoiceClient(http, engine, nullptr)
{
    peers_ = [this](std::shared_ptr<JitterBuffer> sound)
    { return createPeer(stun_, std::move(sound)); };
}

VoiceClient::VoiceClient(net::HttpClient& http, audio::AudioEngine* engine, PeerFactory peers)
    : http_(http), engine_(engine), peers_(std::move(peers)), input_(engine),
      group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

VoiceClient::~VoiceClient()
{
    stop();
}

void VoiceClient::stop()
{
    input_.setSinks({});
    input_.release();
    while(!links_.empty())
    {
        drop(links_.begin());
    }
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    std::vector<FeaturesReply> stale;
    featureReplies_.drain(stale);
    features_.clear();
    parcelWaitSince_.reset();
    lastPlacement_.clear();
    nextUpdate_ = {};
    talking_ = false; // starts muted again
    state_ = State::Off;
}

void VoiceClient::setVolume(float volume)
{
    volume_ = volume;
    for(const auto& [key, link] : links_)
    {
        if(engine_ && link.voice)
        {
            engine_->setGain(link.voice, volume_);
        }
    }
}

void VoiceClient::drop(std::map<Key, Link>::iterator link)
{
    if(engine_ && link->second.voice)
    {
        engine_->stop(link->second.voice);
    }
    links_.erase(link);
}

void VoiceClient::updateFeatures(const World& world, Clock::time_point now)
{
    std::vector<FeaturesReply> replies;
    featureReplies_.drain(replies);
    for(const FeaturesReply& r : replies)
    {
        const auto it = features_.find(r.region);
        if(it == features_.end() || it->second.url != r.url)
        {
            continue;
        }
        Features& f = it->second;
        f.asked = false;
        const auto body =
            r.response.ok() ? llsd::parse(r.response.bodyText()) : std::unexpected(std::string());
        if(!body || !body->isMap())
        {
            f.retryAt = now + kFeaturesRetry;
            continue;
        }
        f.known = true;
        f.webrtc = (*body)["VoiceServerType"].asString() == "webrtc";
    }

    std::set<uint64_t> here;
    for(const Region& r : world.regions)
    {
        here.insert(r.handle);
        Features& f = features_[r.handle];
        if(f.region != r.id || (f.url != r.features && !r.features.empty()))
        {
            f = Features{};
            f.region = r.id;
            f.url = r.features;
        }
        if(f.known || f.asked || f.url.empty() || now < f.retryAt)
        {
            continue;
        }
        f.asked = true;
        net::HttpRequest request{.url = f.url};
        request.headers.add("Accept", std::string(llsd::kXmlContentType));
        group_->send(std::move(request),
                     [this, handle = r.handle, url = f.url](net::HttpResponse&& response)
                     { featureReplies_.push({handle, url, std::move(response)}); });
    }
    std::erase_if(features_, [&](const auto& f) { return !here.contains(f.first); });
}

std::optional<bool> VoiceClient::hasVoice(uint64_t region) const
{
    const auto it = features_.find(region);
    return it != features_.end() && it->second.known ? std::optional(it->second.webrtc)
                                                     : std::nullopt;
}

std::optional<std::vector<VoiceClient::Key>> VoiceClient::wanted(const World& world,
                                                                 Clock::time_point now)
{
    const auto find = [&](uint64_t handle)
    {
        const auto it = std::ranges::find(world.regions, handle, &Region::handle);
        return it == world.regions.end() ? nullptr : &*it;
    };
    const Region* region = find(world.agentRegion);
    if(!region)
    {
        return std::nullopt; // between regions
    }
    const std::optional<bool> webrtc = hasVoice(region->handle);
    if(!webrtc)
    {
        return std::nullopt;
    }
    if(!world.parcel)
    {
        if(!parcelWaitSince_)
        {
            parcelWaitSince_ = now;
        }
        if(now - *parcelWaitSince_ < kParcelWait)
        {
            return std::nullopt;
        }
    }
    else
    {
        parcelWaitSince_.reset();
    }
    if(!*webrtc || !region->allowsVoice || (world.parcel && !world.parcel->allowsVoice))
    {
        return std::vector<Key>{};
    }
    if(world.parcel && !world.parcel->estateChannel)
    {
        return std::vector<Key>{{region->handle, world.parcel->localId}};
    }
    // The estate's: here, and 100 m away each way (twice as far as anyone's
    // heard, since the camera may be 50 m off).
    std::set<uint64_t> handles{region->handle};
    const double d = 2.0 * kMaxListenerDistance;
    const double s = d * std::sqrt(0.5);
    for(const glm::dvec2 o :
        {glm::dvec2(d, 0.0), glm::dvec2(s, s), glm::dvec2(0.0, d), glm::dvec2(-s, s),
         glm::dvec2(-d, 0.0), glm::dvec2(-s, -s), glm::dvec2(0.0, -d), glm::dvec2(s, -s)})
    {
        const double x = std::floor((world.placement.speaker.x + o.x) / 256.0) * 256.0;
        const double y = std::floor((world.placement.speaker.y + o.y) / 256.0) * 256.0;
        if(x < 0.0 || y < 0.0)
        {
            continue;
        }
        const uint64_t handle = static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(y);
        if(find(handle) && hasVoice(handle).value_or(false))
        {
            handles.insert(handle);
        }
    }
    std::vector<Key> keys;
    for(const uint64_t h : handles)
    {
        keys.push_back({h, -1});
    }
    return keys;
}

void VoiceClient::update(const World& world, Clock::time_point now)
{
    if(!enabled_)
    {
        if(state_ != State::Off)
        {
            stop();
        }
        return;
    }
    if(now < nextUpdate_)
    {
        return;
    }
    nextUpdate_ = now + kInterval;
    self_ = world.self;
    stun_ = stunServers(world.loginUri);
    updateFeatures(world, now);

    // Connections there should be, and not.
    const auto keys = wanted(world, now);
    const auto regionOf = [&](uint64_t handle)
    {
        const auto it = std::ranges::find(world.regions, handle, &Region::handle);
        return it == world.regions.end() ? nullptr : &*it;
    };
    for(auto it = links_.begin(); it != links_.end();)
    {
        const auto next = std::next(it);
        if((keys && std::ranges::find(*keys, it->first) == keys->end()) ||
           !regionOf(it->first.region))
        {
            log::info("voice: leaving a channel");
            drop(it);
        }
        it = next;
    }
    if(keys)
    {
        for(const Key& key : *keys)
        {
            if(links_.contains(key))
            {
                continue;
            }
            const Region* r = regionOf(key.region);
            links_[key].connection = std::make_unique<VoiceConnection>(
                http_, peers_, key.parcel < 0 ? r->name : r->name + " (parcel)",
                key.parcel < 0 ? std::nullopt : std::optional(key.parcel));
        }
    }

    const std::string placement = placementMessage(world.placement);
    const bool moved = placement != lastPlacement_;
    lastPlacement_ = placement;
    bool connected = false;
    for(auto& [key, link] : links_)
    {
        VoiceConnection& c = *link.connection;
        const Region* r = regionOf(key.region);
        c.setCapabilities(r->provision, r->signaling);
        c.setPrimary(key.region == world.agentRegion);
        c.update(now);
        if(c.up() && (c.takeCameUp() || moved))
        {
            c.send(placement);
        }
        if(link.playing != c.sound())
        {
            if(engine_ && link.voice)
            {
                engine_->stop(link.voice);
            }
            link.voice = 0;
            link.playing = c.sound();
            if(engine_ && link.playing)
            {
                audio::AudioEngine::PlayParams params;
                params.positional = false;
                params.gain = volume_;
                link.voice = engine_->playStream(link.playing, params);
            }
        }
        connected = connected || (c.up() && c.primary());
    }
    state_ = connected               ? State::Connected
             : keys && keys->empty() ? State::NotAllowed
                                     : State::Connecting;
    updateSinks();
}

void VoiceClient::updateSinks()
{
    std::vector<VoiceInput::Sink> sinks;
    for(const auto& [key, link] : links_)
    {
        if(link.connection->up())
        {
            sinks.push_back({link.connection->peer(), link.connection->primary()});
        }
    }
    const bool none = links_.empty();
    input_.setSinks(std::move(sinks));
    // Where there's no voice, the microphone's closed (opened again with
    // the next channel, if it was on).
    if(none)
    {
        input_.release();
    }
    else if(input_.talking() != (talking_ && state_ == State::Connected))
    {
        input_.setTalking(talking_ && state_ == State::Connected);
    }
}

bool VoiceClient::failing() const
{
    return std::ranges::any_of(links_,
                               [](const auto& l) { return l.second.connection->failing(); });
}

bool VoiceClient::moderatorMuted() const
{
    return std::ranges::any_of(links_,
                               [this](const auto& l)
                               {
                                   const auto& people = l.second.connection->participants();
                                   const auto it = people.find(self_);
                                   return it != people.end() && it->second.moderatorMuted;
                               });
}

size_t VoiceClient::participants() const
{
    std::set<Uuid> ids;
    for(const auto& [key, link] : links_)
    {
        for(const auto& [id, p] : link.connection->participants())
        {
            ids.insert(id);
        }
    }
    return ids.size();
}

std::optional<VoiceClient::Status> VoiceClient::status(const Uuid& id) const
{
    if(id == self_)
    {
        if(!input_.talking() || !input_.open())
        {
            return std::nullopt;
        }
        const float rms = input_.power();
        // On the servers' scale, by loudness (llvoicewebrtc.cpp
        // updateOwnVolume: LEVEL_START_POINT, LEVEL_SCALE).
        const float power = 0.18f + 0.005f * 20.0f * std::log10(std::max(rms * 32768.0f, 1.0f));
        return Status{rms > kSpeaking, power, moderatorMuted()};
    }
    for(const auto& [key, link] : links_)
    {
        const auto& people = link.connection->participants();
        if(const auto it = people.find(id); it != people.end())
        {
            const auto& p = it->second;
            return Status{p.speaking && !p.moderatorMuted, p.power, p.moderatorMuted};
        }
    }
    return std::nullopt;
}

} // namespace viewer::voice
