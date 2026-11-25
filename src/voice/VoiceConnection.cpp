#include "viewer/voice/VoiceConnection.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <random>
#include <utility>

namespace viewer::voice
{

namespace
{

using namespace std::chrono_literals;

constexpr auto kTimeout = 30s;
constexpr float kMaxRetrySeconds = 10.0f;

// 0.5 to 1.5 s more each time, so everyone doesn't come back at once.
float someSeconds()
{
    std::random_device random;
    return 0.5f + std::uniform_real_distribution<float>(0.0f, 1.0f)(random);
}

std::string why(const net::HttpResponse& r)
{
    return r.error != net::HttpError::None ? std::string(net::errorName(r.error))
                                           : std::format("HTTP {}", r.status);
}

} // namespace

VoiceConnection::VoiceConnection(net::HttpClient& http, PeerFactory peers, std::string name,
                                 std::optional<int> parcel)
    : http_(http), peers_(std::move(peers)), name_(std::move(name)), parcel_(parcel),
      retrySeconds_(someSeconds()), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

VoiceConnection::~VoiceConnection()
{
    close();
}

void VoiceConnection::setCapabilities(const std::string& provision, const std::string& signaling)
{
    provisionUrl_ = provision;
    signalingUrl_ = signaling;
}

void VoiceConnection::setPrimary(bool primary)
{
    if(primary == primary_)
    {
        return;
    }
    primary_ = primary;
    send(joinMessage(primary_));
}

net::HttpRequest VoiceConnection::post(const std::string& url, const llsd::Value& body) const
{
    net::HttpRequest r{.method = net::HttpMethod::Post, .url = url};
    r.headers.add("Content-Type", std::string(llsd::kXmlContentType));
    r.headers.add("Accept", std::string(llsd::kXmlContentType));
    const std::string xml = llsd::toXml(body);
    r.body.assign(xml.begin(), xml.end());
    return r;
}

void VoiceConnection::start(Clock::time_point now)
{
    close();
    sound_ = std::make_shared<JitterBuffer>();
    peer_ = peers_(sound_);
    deadline_ = now + kTimeout;
    state_ = State::Offering;
    if(!peer_)
    {
        fail("no WebRTC", now);
    }
}

void VoiceConnection::close()
{
    // Replies still to come aren't ours any more.
    group_ = std::make_unique<net::HttpRequestGroup>(http_);
    std::vector<Reply> stale;
    replies_.drain(stale);
    if(!viewerSession_.empty() && !provisionUrl_.empty())
    {
        http_.send(post(provisionUrl_, logoutRequest(viewerSession_)), [](net::HttpResponse&&) {});
    }
    viewerSession_.clear();
    peer_.reset();
    sound_.reset();
    offer_.clear();
    candidates_.clear();
    gathered_ = false;
    channelOpen_ = false;
    connected_ = false;
    signaling_ = false;
    cameUp_ = false;
    participants_.clear();
}

void VoiceConnection::fail(const std::string& reason, Clock::time_point now)
{
    log::warn("voice: {}: {}; trying again in {:.1f} s", name_, reason, retrySeconds_);
    close();
    state_ = State::Retry;
    retryAt_ = now + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<float>(retrySeconds_));
    retrySeconds_ = std::min(retrySeconds_ + someSeconds(), kMaxRetrySeconds);
}

void VoiceConnection::send(const std::string& message)
{
    if(state_ == State::Up && peer_)
    {
        peer_->sendMessage(message);
    }
}

void VoiceConnection::update(Clock::time_point now)
{
    if(state_ == State::Start || (state_ == State::Retry && now >= retryAt_))
    {
        start(now);
    }

    std::vector<Reply> replies;
    replies_.drain(replies);
    for(const Reply& r : replies)
    {
        if(r.signaling)
        {
            signaling_ = false;
            if(!r.response.ok())
            {
                fail("sending candidates: " + why(r.response), now);
                return;
            }
            continue;
        }
        if(!r.response.ok())
        {
            fail(r.response.status == 409   ? std::string("the channel is full")
                 : r.response.status == 401 ? std::string("not let into the channel")
                                            : "asking for it: " + why(r.response),
                 now);
            return;
        }
        const auto body = llsd::parse(r.response.bodyText());
        const auto reply = body ? parseProvisionReply(*body) : std::nullopt;
        if(reply)
        {
            viewerSession_ = reply->viewerSession;
        }
        if(!reply || !peer_->setAnswer(reply->sdp))
        {
            fail("not an answer", now);
            return;
        }
        state_ = State::Connecting;
        deadline_ = now + kTimeout;
    }

    if(peer_)
    {
        for(const PeerEvent& event : peer_->takeEvents())
        {
            handle(event, now);
            if(!peer_)
            {
                return; // failed
            }
        }
    }

    if(state_ == State::Offering && !offer_.empty() && !provisionUrl_.empty())
    {
        state_ = State::Requesting;
        deadline_ = now + kTimeout + 5s; // the request's own comes first
        group_->send(post(provisionUrl_, provisionRequest(offer_, parcel_)),
                     [this](net::HttpResponse&& response)
                     { replies_.push({false, std::move(response)}); });
    }
    if(!viewerSession_.empty())
    {
        sendCandidates();
    }
    if(state_ == State::Connecting && connected_)
    {
        state_ = State::Joining;
        deadline_ = now + kTimeout;
    }
    if(state_ == State::Joining && channelOpen_)
    {
        state_ = State::Up;
        retrySeconds_ = someSeconds();
        cameUp_ = true;
        log::info("voice: {}: connected", name_);
        send(joinMessage(primary_));
    }
    // (A region's capabilities may take a while: no time limit on those.)
    const bool waiting = state_ == State::Offering && provisionUrl_.empty();
    if(!waiting && state_ != State::Up && state_ != State::Retry && now > deadline_)
    {
        fail("timed out", now);
    }
}

void VoiceConnection::handle(const PeerEvent& event, Clock::time_point now)
{
    switch(event.kind)
    {
    case PeerEvent::Kind::Offer:
        offer_ = event.text;
        break;
    case PeerEvent::Kind::Candidate:
        candidates_.push_back({event.mid, event.text});
        break;
    case PeerEvent::Kind::Gathered:
        gathered_ = true;
        break;
    case PeerEvent::Kind::Connected:
        connected_ = true;
        break;
    case PeerEvent::Kind::Lost:
        fail("the connection was lost", now);
        break;
    case PeerEvent::Kind::ChannelOpen:
        channelOpen_ = true;
        break;
    case PeerEvent::Kind::Message:
        heard(event.text);
        break;
    }
}

void VoiceConnection::sendCandidates()
{
    if(signaling_ || signalingUrl_.empty() || (candidates_.empty() && !gathered_))
    {
        return;
    }
    const llsd::Value body = candidatesRequest(viewerSession_, candidates_);
    if(candidates_.empty())
    {
        gathered_ = false; // said
    }
    candidates_.clear();
    signaling_ = true;
    group_->send(post(signalingUrl_, body), [this](net::HttpResponse&& response)
                 { replies_.push({true, std::move(response)}); });
}

void VoiceConnection::heard(const std::string& message)
{
    for(const ParticipantUpdate& u : parseParticipants(message))
    {
        auto it = participants_.find(u.id);
        // Someone by a region border is connected to several servers: only
        // the one they're in counts.
        if(it == participants_.end())
        {
            if(!u.joined || !u.primary)
            {
                continue;
            }
            it = participants_.emplace(u.id, Participant{}).first;
        }
        if(u.left)
        {
            participants_.erase(it);
            continue;
        }
        Participant& p = it->second;
        p.power = u.power.value_or(p.power);
        p.speaking = u.speaking.value_or(p.speaking);
        if(primary_ || u.primary)
        {
            p.moderatorMuted = u.moderatorMuted.value_or(p.moderatorMuted);
        }
    }
}

} // namespace viewer::voice
