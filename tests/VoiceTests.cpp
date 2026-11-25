#include "MicrophoneList.hpp"
#include "VoiceIndicators.hpp"

#include "viewer/audio/Microphone.hpp"
#include "viewer/voice/VoiceClient.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>
#include <opus.h>
#include <rtc/rtc.hpp>

#include <cmath>
#include <numbers>
#include <thread>

using namespace viewer;
using namespace viewer::voice;
using namespace std::chrono_literals;

namespace
{

std::vector<uint8_t> rtp(uint16_t sequence, std::span<const uint8_t> payload, uint32_t source = 7)
{
    std::vector<uint8_t> b{0x80, 111, uint8_t(sequence >> 8), uint8_t(sequence)};
    const uint32_t t = sequence * 960u;
    for(const uint32_t v : {t, source})
    {
        for(int s = 24; s >= 0; s -= 8)
        {
            b.push_back(uint8_t(v >> s));
        }
    }
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// 20 ms packets of a 440 Hz tone.
std::vector<std::vector<uint8_t>> tone(int packets)
{
    int error = 0;
    OpusEncoder* e = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(10));
    std::vector<std::vector<uint8_t>> out;
    int at = 0;
    for(int p = 0; p < packets; p++)
    {
        float frame[960];
        for(float& s : frame)
        {
            s = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * float(at++) / 48000.0f);
        }
        std::vector<uint8_t> b(1275);
        b.resize(size_t(opus_encode_float(e, frame, 960, b.data(), 1275)));
        out.push_back(std::move(b));
    }
    opus_encoder_destroy(e);
    return out;
}

// The loudness of the next `ms` milliseconds.
float loudness(JitterBuffer& j, int ms)
{
    float sum = 0.0f;
    int n = 0;
    for(int i = 0; i < ms / 10; i++)
    {
        float out[960];
        CHECK(j.read(out, 480) == 480);
        for(const float s : out)
        {
            sum += s * s;
            n++;
        }
    }
    return std::sqrt(sum / float(n));
}

class FakeHttp final : public net::HttpClient
{
public:
    struct Sent
    {
        net::HttpRequest request;
        net::HttpCallback callback;
        llsd::Value body() const { return *llsd::parse(llsd::asText(request.body)); }
    };
    net::HttpRequestId send(net::HttpRequest request, net::HttpCallback callback) override
    {
        sent.push_back({std::move(request), std::move(callback)});
        return sent.size();
    }
    bool cancel(net::HttpRequestId) override { return false; }
    void answer(size_t i, const llsd::Value& body, int status = 200)
    {
        net::HttpResponse r;
        r.requestId = i + 1;
        r.status = status;
        const std::string xml = llsd::toXml(body);
        r.body.assign(xml.begin(), xml.end());
        const auto callback = sent[i].callback;
        callback(std::move(r));
    }
    // The last request to a URL.
    std::optional<size_t> find(std::string_view url) const
    {
        for(size_t i = sent.size(); i-- > 0;)
        {
            if(sent[i].request.url == url)
            {
                return i;
            }
        }
        return std::nullopt;
    }
    std::vector<Sent> sent;
};

struct FakePeer final : Peer
{
    std::vector<PeerEvent> events;
    std::vector<std::string> messages;
    std::string answer;

    std::vector<PeerEvent> takeEvents() override { return std::exchange(events, {}); }
    bool setAnswer(const std::string& sdp) override
    {
        answer = sdp;
        return true;
    }
    void sendMessage(std::string_view text) override { messages.emplace_back(text); }
    void sendAudio(std::span<const uint8_t>, uint32_t) override {}
};

const Uuid kSelf = *Uuid::parse("11111111-1111-1111-1111-111111111111");
const Uuid kOther = *Uuid::parse("22222222-2222-2222-2222-222222222222");
constexpr uint64_t kHere = uint64_t{256000} << 32 | 256000;

VoiceClient::World world()
{
    VoiceClient::World w;
    w.self = kSelf;
    w.agentRegion = kHere;
    w.regions.push_back({kHere, kOther, "Here", true, "features", "provision", "signaling"});
    w.parcel = VoiceClient::Parcel{5, true, true};
    w.placement.speaker = {256128.0, 256128.0, 25.0};
    w.placement.listener = w.placement.speaker;
    return w;
}

llsd::Value webrtcFeatures()
{
    llsd::Value v;
    v["VoiceServerType"] = "webrtc";
    return v;
}

llsd::Value answerBody()
{
    llsd::Value v;
    v["viewer_session"] = "session";
    v["jsep"]["type"] = "answer";
    v["jsep"]["sdp"] = "the answer";
    return v;
}

} // namespace

TEST_CASE("Voice: RTP packets")
{
    const std::vector<uint8_t> payload{1, 2, 3};
    const auto bytes = rtp(0x1234, payload);
    const auto p = parseRtp(bytes);
    REQUIRE(p);
    CHECK(p->sequence == 0x1234);
    CHECK(p->payloadType == 111);
    CHECK(p->payload.size() == 3);

    auto padded = bytes; // two bytes of padding
    padded[0] |= 0x20;
    padded.push_back(0);
    padded.push_back(2);
    CHECK(parseRtp(padded)->payload.size() == 3);

    std::vector<uint8_t> extended(bytes.begin(), bytes.begin() + 12);
    extended[0] |= 0x10;
    extended.insert(extended.end(), {0xBE, 0xDE, 0, 1, 9, 9, 9, 9, 1, 2});
    CHECK(parseRtp(extended)->payload.size() == 2);
    extended[15] = 200; // an extension longer than the packet
    CHECK_FALSE(parseRtp(extended));

    auto rtcp = bytes;
    rtcp[1] = 200;
    CHECK_FALSE(parseRtp(rtcp));
    CHECK_FALSE(parseRtp(std::span(bytes).first(11)));
}

TEST_CASE("Voice: the jitter buffer plays packets in order, after a little")
{
    const auto packets = tone(30);
    JitterBuffer j;
    CHECK(loudness(j, 20) == 0.0f);
    j.push(rtp(65530, packets[0]));
    CHECK(loudness(j, 20) == 0.0f); // not enough yet
    for(size_t i = 1; i < 30; i++)  // the sequence wraps; each pair swapped
    {
        const size_t at = i % 2 == 1 && i + 1 < 30 ? i + 1 : i % 2 == 0 ? i - 1 : i;
        j.push(rtp(uint16_t(65530 + at), packets[at]));
    }
    CHECK(loudness(j, 400) > 0.2f);
    CHECK(j.stats().concealed == 0);
    CHECK(j.stats().late == 0);
    j.push(rtp(65530, packets[0]));
    CHECK(j.stats().late == 1);
}

TEST_CASE("Voice: lost packets are made up, and running dry waits for more")
{
    const auto packets = tone(40);
    JitterBuffer j;
    for(size_t i = 0; i < 20; i++)
    {
        if(i != 8 && i != 9)
        {
            j.push(rtp(uint16_t(i), packets[i]));
        }
    }
    loudness(j, 100);
    CHECK(loudness(j, 200) > 0.15f);
    CHECK(j.stats().concealed == 2);
    loudness(j, 400);
    CHECK(j.stats().underruns == 1);
    CHECK(loudness(j, 50) == 0.0f);
    const float target = j.stats().targetMs;
    CHECK(target > 60.0f);
    for(size_t i = 20; i < 40; i++)
    {
        j.push(rtp(uint16_t(i + 200), packets[i]));
    }
    CHECK(loudness(j, 200) > 0.2f); // after a long gap, at once
    // Another source starts anew.
    for(size_t i = 0; i < 10; i++)
    {
        j.push(rtp(uint16_t(i), packets[i], 99));
    }
    CHECK(loudness(j, 150) > 0.15f);
    // Junk is ignored.
    j.push(rtp(50, std::vector<uint8_t>{}));
    j.push(std::vector<uint8_t>(5, 0xFF));
}

TEST_CASE("Voice: what's held too long is played off")
{
    const auto packets = tone(400);
    JitterBuffer j;
    size_t at = 0;
    for(; at < 20; at++)
    {
        j.push(rtp(uint16_t(at), packets[at])); // 400 ms at once
    }
    for(; at < 400; at++)
    {
        loudness(j, 20);
        j.push(rtp(uint16_t(at), packets[at]));
    }
    CHECK(j.stats().skipped >= 3);
    CHECK(j.stats().heldMs < 380.0f);
}

TEST_CASE("Voice: messages")
{
    CHECK(joinMessage(true) == R"({"j":{"p":true}})");
    CHECK(joinMessage(false) == R"({"j":{}})");
    Placement p;
    p.speaker = {100.0, 200.0, 30.5};
    p.listener = {200.0, 200.0, 30.5}; // too far: held 50 m away
    const auto m = llsd::fromJson(placementMessage(p));
    REQUIRE(m);
    CHECK((*m)["sp"]["z"].asInteger() == 3050);
    CHECK((*m)["lp"]["x"].asInteger() == 15000);
    CHECK((*m)["sh"]["w"].asInteger() == 100);

    const auto u = parseParticipants(
        R"({"22222222-2222-2222-2222-222222222222":{"j":{"p":true},"p":64,"v":true},
            "junk":{"l":true},"00000000-0000-0000-0000-000000000000":{"l":true}})");
    REQUIRE(u.size() == 1);
    CHECK(u[0].id == kOther);
    CHECK(u[0].primary);
    CHECK(*u[0].power == doctest::Approx(0.5f));
    CHECK(*u[0].speaking);
    CHECK_FALSE(u[0].moderatorMuted);
    CHECK(parseParticipants("[1").empty());

    CHECK(stunServers("https://login.agni.lindenlab.com/cgi-bin/login.cgi").size() == 3);
    CHECK(stunServers("https://login.aditi.lindenlab.com/cgi-bin/login.cgi")[0] ==
          "stun:stun1.aditi.secondlife.io:3478");
    CHECK(stunServers("http://localhost:9000").empty());
}

TEST_CASE("Voice: connecting to the estate's channel, and leaving")
{
    FakeHttp http;
    std::vector<std::shared_ptr<FakePeer>> peers;
    {
        VoiceClient client(http, nullptr,
                           [&](const std::shared_ptr<JitterBuffer>&)
                           {
                               peers.push_back(std::make_shared<FakePeer>());
                               return peers.back();
                           });
        auto now = VoiceClient::Clock::now();
        auto w = world();
        const auto step = [&]
        {
            now += 150ms;
            client.update(w, now);
        };
        step();
        REQUIRE(http.find("features"));
        CHECK(peers.empty());
        http.answer(*http.find("features"), webrtcFeatures());
        step();
        REQUIRE(peers.size() == 1);
        CHECK(client.state() == VoiceClient::State::Connecting);

        // Our offer's posted; candidates wait for the answer.
        peers[0]->events = {{PeerEvent::Kind::Offer, "the offer", {}},
                            {PeerEvent::Kind::Candidate, "candidate:1", "0"}};
        step();
        REQUIRE(http.find("provision"));
        const auto body = http.sent[*http.find("provision")].body();
        CHECK(body["jsep"]["sdp"].asString() == "the offer");
        CHECK(body["channel_type"].asString() == "local");
        CHECK_FALSE(body.has("parcel_local_id"));
        CHECK_FALSE(http.find("signaling"));
        http.answer(*http.find("provision"), answerBody());
        step();
        CHECK(peers[0]->answer == "the answer");
        REQUIRE(http.find("signaling"));
        auto signal = http.sent[*http.find("signaling")].body();
        CHECK(signal["viewer_session"].asString() == "session");
        CHECK(signal["candidates"][0]["candidate"].asString() == "candidate:1");
        http.answer(*http.find("signaling"), {});
        peers[0]->events = {{PeerEvent::Kind::Gathered, {}, {}}};
        step();
        signal = http.sent[*http.find("signaling")].body();
        CHECK(signal["candidate"]["completed"].asBoolean());

        peers[0]->events = {{PeerEvent::Kind::Connected, {}, {}},
                            {PeerEvent::Kind::ChannelOpen, {}, {}}};
        step();
        CHECK(client.state() == VoiceClient::State::Connected);
        REQUIRE(peers[0]->messages.size() == 2);
        CHECK(peers[0]->messages[0] == joinMessage(true));
        CHECK(peers[0]->messages[1].starts_with(R"({"sp")"));
        step(); // not moved: nothing said
        CHECK(peers[0]->messages.size() == 2);
        w.placement.speaker.x += 1.0;
        step();
        CHECK(peers[0]->messages.size() == 3);

        // Others.
        const std::string id = kOther.toString();
        peers[0]->events = {
            {PeerEvent::Kind::Message, R"({")" + id + R"(":{"j":{"p":true}}})", {}},
            {PeerEvent::Kind::Message, R"({")" + id + R"(":{"p":32,"v":true}})", {}}};
        step();
        CHECK(client.participants() == 1);
        REQUIRE(client.status(kOther));
        CHECK(client.status(kOther)->speaking);
        CHECK(client.status(kOther)->power == doctest::Approx(0.25f));
        CHECK_FALSE(client.status(kSelf)); // the microphone's off
        peers[0]->events = {{PeerEvent::Kind::Message, R"({")" + id + R"(":{"l":true}})", {}}};
        step();
        CHECK_FALSE(client.status(kOther));
        CHECK(client.participants() == 0);

        // Onto a parcel with its own channel: the estate's is left.
        w.parcel = VoiceClient::Parcel{9, true, false};
        step();
        REQUIRE(peers.size() == 2);
        CHECK(http.sent.back().body()["logout"].asBoolean());
        peers[1]->events = {{PeerEvent::Kind::Offer, "another", {}}};
        step();
        CHECK(http.sent.back().body()["parcel_local_id"].asInteger() == 9);

        // Refused: tried again later.
        http.answer(http.sent.size() - 1, {}, 403);
        step();
        CHECK(client.failing());
        now += 3s;
        step();
        CHECK(peers.size() == 3);

        // No voice here.
        w.parcel->allowsVoice = false;
        step();
        CHECK(client.state() == VoiceClient::State::NotAllowed);
        client.setEnabled(false);
        step();
        CHECK(client.state() == VoiceClient::State::Off);
    }
}

TEST_CASE("Voice: neighboring regions with voice, and which one hears us")
{
    FakeHttp http;
    std::vector<std::shared_ptr<FakePeer>> peers;
    VoiceClient client(http, nullptr,
                       [&](const std::shared_ptr<JitterBuffer>&)
                       {
                           peers.push_back(std::make_shared<FakePeer>());
                           return peers.back();
                       });
    auto now = VoiceClient::Clock::now();
    auto w = world();
    const uint64_t east = uint64_t{256256} << 32 | 256000;
    const uint64_t west = uint64_t{255744} << 32 | 256000; // no WebRTC
    w.regions.push_back({east, kSelf, "East", true, "features2", "provision2", "signaling2"});
    w.regions.push_back({west, kSelf, "West", true, "features3", "provision3", "signaling3"});
    const auto step = [&]
    {
        now += 150ms;
        client.update(w, now);
    };
    step();
    http.answer(*http.find("features"), webrtcFeatures());
    http.answer(*http.find("features2"), webrtcFeatures());
    http.answer(*http.find("features3"), llsd::Value(llsd::Map{}));
    step();
    CHECK(peers.size() == 1); // in the middle: none near
    w.placement.speaker.x = 256200.0;
    step();
    CHECK(peers.size() == 2);
    w.placement.speaker.x = 256050.0; // the west's near, without voice
    step();
    CHECK(peers.size() == 2);
    w.placement.speaker.x = 256200.0;
    step();
    const auto bring = [&](FakePeer& peer, std::string_view provision)
    {
        peer.events = {{PeerEvent::Kind::Offer, "offer", {}}};
        step();
        http.answer(*http.find(provision), answerBody());
        peer.events = {{PeerEvent::Kind::Connected, {}, {}},
                       {PeerEvent::Kind::ChannelOpen, {}, {}}};
        step();
    };
    REQUIRE(peers.size() == 3);
    bring(*peers[0], "provision");
    bring(*peers[2], "provision2");
    CHECK(peers[0]->messages[0] == joinMessage(true));
    CHECK(peers[2]->messages[0] == joinMessage(false));
    // Over the border: the other one now.
    w.agentRegion = east;
    w.placement.speaker.x = 256260.0;
    step();
    CHECK(peers.size() == 3);
    CHECK(std::ranges::find(peers[0]->messages, joinMessage(false)) != peers[0]->messages.end());
    CHECK(std::ranges::find(peers[2]->messages, joinMessage(true)) != peers[2]->messages.end());
}

TEST_CASE("Voice: microphones to choose from")
{
    const std::vector<std::string> names{"Webcam Mic", "Webcam", "Webcam"};
    auto c = microphoneChoices(names, "");
    REQUIRE(c.size() == 3); // the default, and each once
    CHECK(c[0].value.empty());
    c = microphoneChoices(names, "Webcam");
    CHECK(c.size() == 3);
    c = microphoneChoices(names, "Headset"); // unplugged: kept, and said
    REQUIRE(c.size() == 4);
    CHECK(c[3].value == "Headset");
    CHECK(c[3].label.find("not connected") != std::string::npos);

    using audio::Microphone;
    CHECK(Microphone::match(names, "Webcam") == 1); // exactly, over the first with it
    CHECK(Microphone::match(names, "webcam mic") == 0);
    CHECK_FALSE(Microphone::match(names, "Headset"));
    CHECK_FALSE(Microphone::match(names, ""));
}

TEST_CASE("Voice: indicators rise at once and fall off")
{
    using Status = VoiceClient::Status;
    VoiceIndicators v;
    v.beginFrame(0.1f);
    CHECK_FALSE(v.get(kOther, std::nullopt));
    CHECK(v.get(kOther, Status{})->level == 0.0f); // in voice: a dot
    CHECK(v.get(kOther, Status{true, 0.5f, false})->level == doctest::Approx(0.6f));
    v.beginFrame(0.1f);
    const float falling = v.get(kOther, Status{false, 0.5f, false})->level;
    CHECK(falling > 0.0f);
    CHECK(falling < 0.6f);
    v.beginFrame(0.5f);
    CHECK(v.get(kOther, Status{})->level == 0.0f);
    CHECK(v.get(kOther, Status{true, 0.5f, true})->level == 0.0f); // muted
    // Gray, greener, red.
    CHECK(VoiceIndicators::color(0.1f).r == doctest::Approx(VoiceIndicators::color(0.1f).g));
    CHECK(VoiceIndicators::color(0.6f).g > VoiceIndicators::color(0.6f).r);
    CHECK(VoiceIndicators::color(0.8f).r > VoiceIndicators::color(0.8f).g);
}

TEST_CASE("Voice: the WebRTC offer")
{
    auto peer = createPeer({}, std::make_shared<JitterBuffer>());
    REQUIRE(peer);
    std::string sdp;
    for(int i = 0; i < 100 && sdp.empty(); i++)
    {
        for(const PeerEvent& e : peer->takeEvents())
        {
            if(e.kind == PeerEvent::Kind::Offer)
            {
                sdp = e.text;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    const size_t audio = sdp.find("m=audio");
    const size_t data = sdp.find("m=application");
    INFO(sdp);
    REQUIRE(audio != std::string::npos);
    REQUIRE(data != std::string::npos);
    CHECK(audio < data);
    CHECK(sdp.find("a=rtpmap:111 opus/48000/2") != std::string::npos);
    CHECK(sdp.find("stereo=1") != std::string::npos);
    CHECK(sdp.find("a=sendrecv") != std::string::npos);
    CHECK(sdp.find("a=setup:actpass") != std::string::npos);
    CHECK(sdp.find("a=group:BUNDLE 0 1") != std::string::npos);
    CHECK_FALSE(peer->setAnswer("not an answer"));
}

TEST_CASE("Voice: audio and messages over WebRTC, there and back")
{
    const auto sound = std::make_shared<JitterBuffer>();
    auto peer = createPeer({}, sound);
    REQUIRE(peer);

    // The server's end: sends our audio back, and answers messages.
    rtc::PeerConnection server;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> channel;
    std::mutex mutex;
    std::string answer;
    server.onGatheringStateChange(
        [&](rtc::PeerConnection::GatheringState state)
        {
            if(state == rtc::PeerConnection::GatheringState::Complete)
            {
                std::lock_guard lock(mutex);
                answer = std::string(*server.localDescription());
            }
        });
    server.onTrack(
        [&](std::shared_ptr<rtc::Track> t)
        {
            track = t;
            t->onMessage([raw = t.get()](rtc::binary packet) { raw->send(std::move(packet)); },
                         [](const std::string&) {});
        });
    server.onDataChannel(
        [&](std::shared_ptr<rtc::DataChannel> c)
        {
            channel = c;
            c->onMessage([](const rtc::binary&) {},
                         [raw = c.get()](std::string text) { raw->send("heard " + text); });
        });

    const auto packets = tone(400);
    size_t sent = 0;
    bool offered = false;
    bool answered = false;
    bool connected = false;
    bool asked = false;
    bool heard = false;
    float loudest = 0.0f;
    for(int i = 0; i < 500 && !(heard && loudest > 0.2f); i++)
    {
        for(const PeerEvent& e : peer->takeEvents())
        {
            if(e.kind == PeerEvent::Kind::Offer)
            {
                server.setRemoteDescription(
                    rtc::Description(e.text, rtc::Description::Type::Offer));
                offered = true;
            }
            else if(e.kind == PeerEvent::Kind::Candidate)
            {
                CHECK(e.text.starts_with("candidate:"));
                CHECK(e.mid == "0");
                server.addRemoteCandidate(rtc::Candidate(e.text, e.mid));
            }
            connected = connected || e.kind == PeerEvent::Kind::Connected;
            if(e.kind == PeerEvent::Kind::ChannelOpen)
            {
                peer->sendMessage("hello");
                asked = true;
            }
            heard = heard || (e.kind == PeerEvent::Kind::Message && e.text == "heard hello");
        }
        if(offered && !answered)
        {
            std::lock_guard lock(mutex);
            if(!answer.empty())
            {
                answered = peer->setAnswer(answer);
                CHECK(answered);
            }
        }
        if(connected && sent < packets.size())
        {
            peer->sendAudio(packets[sent], uint32_t(sent * 960));
            sent++;
        }
        loudest = std::max(loudest, loudness(*sound, 20));
        std::this_thread::sleep_for(20ms);
    }
    CHECK(connected);
    CHECK(asked);
    CHECK(heard);
    CHECK(loudest > 0.2f);
    CHECK(sound->stats().received > 0);
    peer.reset();
    server.close();
}
