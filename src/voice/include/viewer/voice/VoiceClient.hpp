#pragma once

// Voice with those nearby (WebRTC), as the official viewer's
// (llvoicewebrtc.cpp LLWebRTCVoiceClient): where the region and parcel allow
// it, a connection to the parcel's own channel, or to the estate's, then also
// to each neighboring region within 100 m that has WebRTC voice. The servers
// mix what we hear for where we listen from; we tell them where that is and
// where we are, ten times a second at most. Only the region we're in hears
// us. (Group, ad-hoc and private calls: not yet.)

#include "viewer/audio/AudioEngine.hpp"
#include "viewer/voice/VoiceConnection.hpp"
#include "viewer/voice/VoiceInput.hpp"

#include <map>
#include <unordered_map>

namespace viewer::voice
{

class VoiceClient
{
public:
    using Clock = VoiceConnection::Clock;

    // `engine` (may be null: nothing's heard) outlives this.
    VoiceClient(net::HttpClient& http, audio::AudioEngine* engine);
    // Tests': peers made by `peers`.
    VoiceClient(net::HttpClient& http, audio::AudioEngine* engine, PeerFactory peers);
    ~VoiceClient();
    VoiceClient(const VoiceClient&) = delete;
    VoiceClient& operator=(const VoiceClient&) = delete;

    struct Region
    {
        uint64_t handle = 0;
        Uuid id;
        std::string name;
        bool allowsVoice = false;
        // Capabilities (empty: not granted, or not known yet).
        std::string features;
        std::string provision;
        std::string signaling;
    };
    struct Parcel
    {
        int localId = 0;
        bool allowsVoice = false;
        bool estateChannel = true;
    };
    struct World
    {
        Uuid self;
        std::string loginUri;
        std::vector<Region> regions;
        uint64_t agentRegion = 0;
        std::optional<Parcel> parcel; // in agentRegion; nothing: not known yet
        Placement placement;
    };
    // Once a frame in the world.
    void update(const World& world, Clock::time_point now);
    // Out of the world: everything closed.
    void stop();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void setVolume(float volume);
    // The microphone, on or off.
    void setTalking(bool talking) { talking_ = talking; }
    bool talking() const { return talking_; }
    VoiceInput& input() { return input_; }

    enum class State : uint8_t
    {
        Off,        // turned off, or not in the world
        NotAllowed, // by the region or parcel, or the region has no WebRTC voice
        Connecting,
        Connected,
    };
    State state() const { return state_; }
    bool failing() const; // trying again
    bool moderatorMuted() const;
    // Who's in voice here.
    size_t participants() const;
    // Someone in it (nothing: not in it). Us: while the microphone's on.
    // Power: 0 to 1, over 0.7 too loud.
    struct Status
    {
        bool speaking = false;
        float power = 0.0f;
        bool moderatorMuted = false;
    };
    std::optional<Status> status(const Uuid& id) const;

private:
    struct Key
    {
        uint64_t region = 0;
        int parcel = -1; // -1: the estate's
        auto operator<=>(const Key&) const = default;
    };
    struct Link
    {
        std::unique_ptr<VoiceConnection> connection;
        std::shared_ptr<JitterBuffer> playing;
        audio::AudioEngine::Voice voice = 0;
    };
    struct Features
    {
        Uuid region;
        std::string url;
        bool known = false;
        bool webrtc = false;
        bool asked = false;
        Clock::time_point retryAt{};
    };
    struct FeaturesReply
    {
        uint64_t region = 0;
        std::string url;
        net::HttpResponse response;
    };
    void updateFeatures(const World& world, Clock::time_point now);
    std::optional<bool> hasVoice(uint64_t region) const;
    // Which connections there should be (nothing: as now, until known).
    std::optional<std::vector<Key>> wanted(const World& world, Clock::time_point now);
    void drop(std::map<Key, Link>::iterator link);
    void updateSinks();

    net::HttpClient& http_;
    audio::AudioEngine* engine_;
    PeerFactory peers_;
    std::vector<std::string> stun_;
    bool enabled_ = true;
    bool talking_ = false;
    float volume_ = 1.0f;
    State state_ = State::Off;
    Uuid self_;
    std::map<Key, Link> links_;
    std::map<uint64_t, Features> features_;
    std::optional<Clock::time_point> parcelWaitSince_;
    Clock::time_point nextUpdate_{};
    std::string lastPlacement_;
    VoiceInput input_;
    MessageQueue<FeaturesReply> featureReplies_;
    std::unique_ptr<net::HttpRequestGroup> group_; // last
};

} // namespace viewer::voice
