// voice::Peer with libdatachannel.

#include "viewer/core/Log.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/voice/Peer.hpp"

#include <rtc/rtc.hpp>

#include <exception>
#include <mutex>
#include <random>

namespace viewer::voice
{

namespace
{

constexpr int kOpus = 111; // its payload type, as everyone's
// Stereo to us: the server places who speaks left and right. As the
// official viewer's offer.
constexpr const char* kOpusProfile = "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;"
                                     "maxplaybackrate=48000;sprop-maxplaybackrate=48000;"
                                     "sprop-maxcapturerate=48000";

using Events = MessageQueue<PeerEvent>;

void logLibrary(rtc::LogLevel level, const std::string& message)
{
    if(level <= rtc::LogLevel::Error)
    {
        log::warn("webrtc: {}", message);
    }
    else
    {
        log::info("webrtc: {}", message);
    }
}

class RtcPeer final : public Peer
{
public:
    RtcPeer(const std::vector<std::string>& stunServers, std::shared_ptr<JitterBuffer> sound)
        : events_(std::make_shared<Events>())
    {
        rtc::Configuration config;
        for(const std::string& url : stunServers)
        {
            config.iceServers.emplace_back(url);
        }
        config.disableAutoNegotiation = true;
        // The official viewer's.
        config.portRangeBegin = 60000;
        config.portRangeEnd = 60100;
        connection_ = std::make_shared<rtc::PeerConnection>(config);

        // Callbacks hold the queue, not this: they may outlast it.
        auto events = events_;
        connection_->onLocalDescription(
            [events](const rtc::Description& d)
            { events->push({PeerEvent::Kind::Offer, std::string(d), {}}); });
        connection_->onLocalCandidate(
            [events](const rtc::Candidate& c)
            { events->push({PeerEvent::Kind::Candidate, c.candidate(), c.mid()}); });
        connection_->onGatheringStateChange(
            [events](rtc::PeerConnection::GatheringState state)
            {
                if(state == rtc::PeerConnection::GatheringState::Complete)
                {
                    events->push({PeerEvent::Kind::Gathered, {}, {}});
                }
            });
        connection_->onStateChange(
            [events](rtc::PeerConnection::State state)
            {
                using State = rtc::PeerConnection::State;
                if(state == State::Connected)
                {
                    events->push({PeerEvent::Kind::Connected, {}, {}});
                }
                else if(state == State::Failed || state == State::Closed)
                {
                    events->push({PeerEvent::Kind::Lost, {}, {}});
                }
            });
        connection_->onDataChannel(
            [events,
             weak = std::weak_ptr(serverChannel_)](const std::shared_ptr<rtc::DataChannel>& channel)
            {
                // The server's own, instead of ours.
                if(const auto holder = weak.lock())
                {
                    listen(*channel, events);
                    std::lock_guard lock(holder->mutex);
                    holder->channel = channel;
                }
            });

        std::random_device random;
        const rtc::SSRC ssrc = random() | 1u;
        rtc::Description::Audio audio("0", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(kOpus, kOpusProfile);
        audio.addSSRC(ssrc, "SLStream", "SLStream", "SLAudio");
        track_ = connection_->addTrack(audio);
        auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "SLStream", static_cast<uint8_t>(kOpus),
            rtc::OpusRtpPacketizer::DefaultClockRate);
        auto handlers = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
        handlers->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        handlers->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track_->setMediaHandler(handlers);
        track_->onMessage(
            [sound = std::move(sound)](const rtc::binary& packet)
            {
                sound->push(
                    std::span(reinterpret_cast<const uint8_t*>(packet.data()), packet.size()));
            },
            [](const std::string&) {});

        channel_ = connection_->createDataChannel("SLData");
        listen(*channel_, events);
        connection_->setLocalDescription(rtc::Description::Type::Offer);
    }

    ~RtcPeer() override
    {
        try
        {
            connection_->resetCallbacks();
            track_->resetCallbacks();
            channel_->resetCallbacks();
            connection_->close();
        }
        catch(const std::exception& e)
        {
            log::warn("webrtc: closing: {}", e.what());
        }
    }

    std::vector<PeerEvent> takeEvents() override
    {
        std::vector<PeerEvent> out;
        events_->drain(out);
        return out;
    }

    bool setAnswer(const std::string& sdp) override
    {
        try
        {
            connection_->setRemoteDescription(
                rtc::Description(sdp, rtc::Description::Type::Answer));
            return true;
        }
        catch(const std::exception& e)
        {
            log::warn("webrtc: the answer: {}", e.what());
            return false;
        }
    }

    void sendMessage(std::string_view text) override
    {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard lock(serverChannel_->mutex);
            channel = serverChannel_->channel;
        }
        if(!channel || !channel->isOpen())
        {
            channel = channel_;
        }
        try
        {
            if(channel->isOpen())
            {
                channel->send(std::string(text));
            }
        }
        catch(const std::exception& e)
        {
            log::warn("webrtc: sending a message: {}", e.what());
        }
    }

    void sendAudio(std::span<const uint8_t> opus, uint32_t timestamp) override
    {
        try
        {
            if(track_->isOpen())
            {
                track_->sendFrame(reinterpret_cast<const rtc::byte*>(opus.data()), opus.size(),
                                  rtc::FrameInfo(timestamp));
            }
        }
        catch(const std::exception&)
        {
            // Closed meanwhile: the state says so too.
        }
    }

private:
    struct Holder
    {
        std::mutex mutex;
        std::shared_ptr<rtc::DataChannel> channel;
    };

    static void listen(rtc::DataChannel& channel, const std::shared_ptr<Events>& events)
    {
        channel.onOpen([events] { events->push({PeerEvent::Kind::ChannelOpen, {}, {}}); });
        channel.onMessage([](const rtc::binary&) {}, [events](std::string text)
                          { events->push({PeerEvent::Kind::Message, std::move(text), {}}); });
    }

    std::shared_ptr<Events> events_;
    std::shared_ptr<Holder> serverChannel_ = std::make_shared<Holder>();
    std::shared_ptr<rtc::PeerConnection> connection_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::DataChannel> channel_;
};

} // namespace

std::shared_ptr<Peer> createPeer(const std::vector<std::string>& stunServers,
                                 std::shared_ptr<JitterBuffer> sound)
{
    static std::once_flag logging; // (the library's logger is the process's)
    std::call_once(logging, [] { rtc::InitLogger(rtc::LogLevel::Warning, logLibrary); });
    try
    {
        return std::make_shared<RtcPeer>(stunServers, std::move(sound));
    }
    catch(const std::exception& e)
    {
        log::warn("webrtc: {}", e.what());
        return nullptr;
    }
}

} // namespace viewer::voice
