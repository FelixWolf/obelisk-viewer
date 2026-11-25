#pragma once

// One WebRTC peer connection: an audio track each way (Opus) and a data
// channel ("SLData"), behind an interface so the library can be swapped.
// Currently libdatachannel (RtcPeer.cpp).
//
// What happens is queued as events; only sendAudio() may be called from
// another thread than the owner's.

#include "viewer/voice/JitterBuffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::voice
{

struct PeerEvent
{
    enum class Kind : uint8_t
    {
        Offer,     // text: our SDP
        Candidate, // text, mid
        Gathered,  // no more candidates
        Connected,
        Lost,        // the connection failed, or closed
        ChannelOpen, // the data channel
        Message,     // text: on the data channel
    };
    Kind kind = Kind::Offer;
    std::string text{};
    std::string mid{};
};

class Peer
{
public:
    virtual ~Peer() = default;
    virtual std::vector<PeerEvent> takeEvents() = 0;
    virtual bool setAnswer(const std::string& sdp) = 0;
    virtual void sendMessage(std::string_view text) = 0;
    virtual void sendAudio(std::span<const uint8_t> opus, uint32_t timestamp) = 0;
};

// What's heard goes into `sound`. Null if it couldn't be made.
std::shared_ptr<Peer> createPeer(const std::vector<std::string>& stunServers,
                                 std::shared_ptr<JitterBuffer> sound);

using PeerFactory = std::function<std::shared_ptr<Peer>(std::shared_ptr<JitterBuffer> sound)>;

} // namespace viewer::voice
