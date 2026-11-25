#pragma once

// One connection to a voice server: a region's estate channel, or a
// parcel's own. As the official viewer's LLVoiceWebRTCConnection: an offer
// is made and posted to the region (ProvisionVoiceAccountRequest), whose
// answer starts the connecting; our candidates follow
// (VoiceSignalingRequest). Once the data channel's open we join on it, say
// where we are, and hear who else is there. Failing, it tries again, later
// each time.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/voice/Messages.hpp"
#include "viewer/voice/Peer.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace viewer::voice
{

class VoiceConnection
{
public:
    using Clock = std::chrono::steady_clock;

    // `parcel`: its channel; nothing: the estate's. `name`: for the log.
    VoiceConnection(net::HttpClient& http, PeerFactory peers, std::string name,
                    std::optional<int> parcel);
    ~VoiceConnection(); // tells the region (a courtesy)
    VoiceConnection(const VoiceConnection&) = delete;
    VoiceConnection& operator=(const VoiceConnection&) = delete;

    // The region's capabilities now (empty: none yet).
    void setCapabilities(const std::string& provision, const std::string& signaling);
    // It's the region we're in: the one that hears us.
    void setPrimary(bool primary);
    bool primary() const { return primary_; }

    void update(Clock::time_point now);
    void send(const std::string& message); // once up

    bool up() const { return state_ == State::Up; }
    bool failing() const { return state_ == State::Retry; }
    // True once after coming up.
    bool takeCameUp() { return std::exchange(cameUp_, false); }
    const std::shared_ptr<Peer>& peer() const { return peer_; }
    // What's heard (replaced when connecting anew).
    const std::shared_ptr<JitterBuffer>& sound() const { return sound_; }

    struct Participant
    {
        float power = 0.0f;
        bool speaking = false;
        bool moderatorMuted = false;
    };
    const std::unordered_map<Uuid, Participant>& participants() const { return participants_; }

private:
    enum class State : uint8_t
    {
        Start,
        Offering,   // our SDP's being made, or the region can't be asked yet
        Requesting, // posted
        Connecting, // answered
        Joining,    // connected; the data channel opens
        Up,
        Retry,
    };
    void start(Clock::time_point now);
    void fail(const std::string& why, Clock::time_point now);
    void close();
    void handle(const PeerEvent& event, Clock::time_point now);
    void sendCandidates();
    void heard(const std::string& message);
    net::HttpRequest post(const std::string& url, const llsd::Value& body) const;

    net::HttpClient& http_;
    PeerFactory peers_;
    std::string name_;
    std::optional<int> parcel_;
    std::string provisionUrl_;
    std::string signalingUrl_;
    bool primary_ = false;
    State state_ = State::Start;
    Clock::time_point deadline_{}; // of this state
    Clock::time_point retryAt_{};
    float retrySeconds_;
    std::shared_ptr<JitterBuffer> sound_;
    std::shared_ptr<Peer> peer_;
    std::string offer_;
    std::string viewerSession_;
    std::vector<Candidate> candidates_;
    bool gathered_ = false;
    bool channelOpen_ = false;
    bool connected_ = false;
    bool signaling_ = false; // a post of candidates is out
    bool cameUp_ = false;
    std::unordered_map<Uuid, Participant> participants_;

    struct Reply
    {
        bool signaling = false;
        net::HttpResponse response;
    };
    MessageQueue<Reply> replies_;
    std::unique_ptr<net::HttpRequestGroup> group_; // last
};

} // namespace viewer::voice
