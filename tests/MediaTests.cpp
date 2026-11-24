#include "TestUtil.hpp"

#include "MusicStream.hpp"
#include "ObjectMedia.hpp"
#include "ParcelInfo.hpp"

#include "viewer/audio/AudioStream.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace viewer;

namespace
{

std::vector<uint8_t> bytes(std::string_view s)
{
    return {s.begin(), s.end()};
}

// Plays a whole stream through read() as the audio thread would; the
// stream's own frames (silence while buffering left out) and the loudest.
struct Played
{
    uint64_t frames = 0;
    float peak = 0.0f;
};

Played playOut(audio::AudioStream& stream)
{
    Played p;
    std::vector<float> out(1024 * 2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(stream.sampleRate() == 0)
        {
            if(stream.state() == audio::AudioStream::State::Failed)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const bool wasBuffering = stream.state() == audio::AudioStream::State::Buffering;
        const uint64_t n = stream.read(out.data(), 1024);
        if(n == 0)
        {
            break;
        }
        float peak = 0.0f;
        for(uint64_t i = 0; i < n * 2; i++)
        {
            peak = std::max(peak, std::abs(out[i]));
        }
        if(peak > 0.0f || !wasBuffering)
        {
            p.frames += n;
        }
        p.peak = std::max(p.peak, peak);
        if(peak == 0.0f && wasBuffering)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return p;
}

} // namespace

TEST_CASE("ICY metadata is taken out of the audio, its title kept")
{
    // Interval 4: four bytes of audio, then a length byte and its block.
    std::string meta = "StreamTitle='Artist - It''s a song';StreamUrl='';";
    meta.resize(64, '\0'); // padded to 16-byte steps
    std::vector<uint8_t> in = bytes("abcd");
    in.push_back(4); // 4 * 16
    in.insert(in.end(), meta.begin(), meta.end());
    for(uint8_t b : bytes("efgh"))
    {
        in.push_back(b);
    }
    in.push_back(0); // no new metadata
    for(uint8_t b : bytes("ij"))
    {
        in.push_back(b);
    }

    // However it's cut up, the same comes out.
    for(size_t piece : {size_t(1), size_t(3), size_t(7), in.size()})
    {
        IcyReader reader(4);
        std::string audio;
        for(size_t at = 0; at < in.size(); at += piece)
        {
            const auto part = std::span(in).subspan(at, std::min(piece, in.size() - at));
            reader.feed(part,
                        [&](std::span<const uint8_t> a) { audio.append(a.begin(), a.end()); });
        }
        CHECK(audio == "abcdefghij");
        const auto title = reader.takeTitle();
        REQUIRE(title.has_value());
        CHECK(*title == "Artist - It''s a song");
        CHECK_FALSE(reader.takeTitle().has_value()); // once
    }

    // No interval: all of it is audio.
    IcyReader plain(0);
    size_t n = 0;
    plain.feed(in, [&](std::span<const uint8_t> a) { n += a.size(); });
    CHECK(n == in.size());
}

TEST_CASE("ICY titles: StreamTitle's value, as UTF-8")
{
    CHECK(icyStreamTitle("StreamTitle='Hello';") == "Hello");
    CHECK(icyStreamTitle("StreamUrl='x';StreamTitle='A';") == "A");
    CHECK(icyStreamTitle(std::string_view("StreamTitle='Pad';\0\0\0", 21)) == "Pad");
    CHECK_FALSE(icyStreamTitle("StreamUrl='x';").has_value());
    // Latin-1 (most old stations) becomes UTF-8; UTF-8 stays.
    CHECK(toUtf8("Caf\xe9") == "Caf\xc3\xa9");
    CHECK(toUtf8("Caf\xc3\xa9") == "Caf\xc3\xa9");
}

TEST_CASE("Audio streams: the format from the Content-Type or the first bytes")
{
    using F = audio::AudioStream::Format;
    CHECK(audio::AudioStream::sniff("audio/mpeg", {}) == F::Mp3);
    CHECK(audio::AudioStream::sniff("Audio/MPEG; charset=x", {}) == F::Mp3);
    CHECK(audio::AudioStream::sniff("application/ogg", {}) == F::Vorbis);
    CHECK(audio::AudioStream::sniff("audio/aacp", {}) == F::Aac);
    CHECK(audio::AudioStream::sniff("", bytes("OggS....")) == F::Vorbis);
    CHECK(audio::AudioStream::sniff("", bytes("ID3\x03")) == F::Mp3);
    const std::vector<uint8_t> mp3 = {0x00, 0xFF, 0xFB, 0x90, 0x00};
    CHECK(audio::AudioStream::sniff("application/octet-stream", mp3) == F::Mp3);
    const std::vector<uint8_t> adts = {0xFF, 0xF1, 0x50, 0x80};
    CHECK(audio::AudioStream::sniff("", adts) == F::Aac);
    CHECK(audio::AudioStream::sniff("", bytes("ab")) == F::Unknown);
}

TEST_CASE("Audio streams: Ogg Vorbis and MP3 decode as they arrive")
{
    for(const char* name : {"tone_440hz_quarter.ogg", "tone_440hz_half.mp3"})
    {
        CAPTURE(name);
        const auto file = test::readTestFile(name);
        REQUIRE(!file.empty());
        audio::AudioStream stream;
        // In small pieces, as a network would.
        for(size_t at = 0; at < file.size(); at += 97)
        {
            stream.push(std::span(file).subspan(at, std::min<size_t>(97, file.size() - at)));
        }
        stream.finish();
        const Played p = playOut(stream);
        CHECK(stream.state() == audio::AudioStream::State::Ended);
        CHECK(stream.error().empty());
        REQUIRE(stream.sampleRate() > 0);
        const double seconds = static_cast<double>(p.frames) / stream.sampleRate();
        CHECK(seconds > 0.2);
        CHECK(seconds < 0.7);
        CHECK(p.peak > 0.05f);
        CHECK(p.peak <= 1.0f);
    }
}

TEST_CASE("Audio streams: what isn't playable fails, saying why")
{
    {
        audio::AudioStream stream("audio/aac");
        stream.finish();
        playOut(stream);
        CHECK(stream.state() == audio::AudioStream::State::Failed);
        CHECK(stream.error().find("AAC") != std::string::npos);
    }
    {
        audio::AudioStream stream;
        const auto html = bytes("<html><body>Not a stream</body></html>");
        stream.push(html);
        stream.finish();
        playOut(stream);
        CHECK(stream.state() == audio::AudioStream::State::Failed);
    }
    {
        // Stopped mid-way: the destructor doesn't wait for bytes.
        audio::AudioStream stream("audio/mpeg");
        stream.push(bytes("ID3"));
    }
}

TEST_CASE("Object media: versions and the capability's answer")
{
    CHECK(mediaVersionNumber("x-mv:0000000003/a2e76fcd-9360-4f6d-a924-000000000003") == 3u);
    CHECK(mediaVersionNumber("x-mv:12") == 12u);
    CHECK_FALSE(mediaVersionNumber("").has_value());
    CHECK_FALSE(mediaVersionNumber("http://example.com/").has_value());

    const auto body = llsd::fromNotation(
        "{'object_id':u3c1f4f3d-0000-4000-8000-000000000001,"
        "'object_media_version':'x-mv:0000000002/00000000-0000-0000-0000-000000000000',"
        "'object_media_data':[!,{'home_url':'http://example.com/','current_url':'',"
        "'auto_play':1,'auto_loop':0,'width_pixels':i1024,'height_pixels':i512,"
        "'whitelist_enable':1,'whitelist':['example.com','*.example.org'],"
        "'perms_interact':i1,'perms_control':i7},!]}");
    REQUIRE(body);
    const auto d = parseObjectMediaResponse(*body);
    REQUIRE(d.has_value());
    CHECK(d->object.toString() == "3c1f4f3d-0000-4000-8000-000000000001");
    CHECK(d->version == 2);
    REQUIRE(d->faces.size() == 3);
    CHECK(d->count() == 1);
    CHECK_FALSE(d->faces[0].has_value());
    REQUIRE(d->faces[1].has_value());
    const MediaEntry& e = *d->faces[1];
    CHECK(e.url() == "http://example.com/");
    CHECK(e.autoPlay);
    CHECK_FALSE(e.autoLoop);
    CHECK(e.width == 1024);
    CHECK(e.height == 512);
    CHECK(e.whitelistEnable);
    CHECK(e.whitelist == std::vector<std::string>{"example.com", "*.example.org"});
    CHECK(e.permsInteract == 1);

    const auto error = llsd::fromNotation("{'error':{'code':i-32603,'message':'no'}}");
    REQUIRE(error);
    CHECK_FALSE(parseObjectMediaResponse(*error).has_value());
}

TEST_CASE("Parcel media: from ParcelProperties, ParcelMediaUpdate and commands")
{
    const auto body = llsd::fromNotation(
        "{'ParcelData':[{'LocalID':i5,'SequenceID':i0,'Name':'Club','MusicURL':"
        "'http://radio.example.com:8000/stream','MediaURL':'http://video.example.com/a.mp4',"
        "'MediaID':u3c1f4f3d-0000-4000-8000-000000000002,'MediaAutoScale':i1}],"
        "'MediaData':[{'MediaType':'video/mp4','MediaDesc':'Movie','MediaWidth':i640,"
        "'MediaHeight':i360,'MediaLoop':i1}]}");
    REQUIRE(body);
    ParcelInfo info;
    info.parcelProperties(1, 1, *body);
    REQUIRE(info.parcel());
    const ParcelMedia& m = info.parcel()->media;
    CHECK(info.parcel()->musicUrl == "http://radio.example.com:8000/stream");
    CHECK(m.url == "http://video.example.com/a.mp4");
    CHECK(m.texture.toString() == "3c1f4f3d-0000-4000-8000-000000000002");
    CHECK(m.autoScale);
    CHECK(m.type == "video/mp4");
    CHECK(m.description == "Movie");
    CHECK(m.width == 640);
    CHECK(m.height == 360);
    CHECK(m.loop);

    ParcelMedia changed = m;
    changed.url = "http://video.example.com/b.mp4";
    const uint64_t before = info.version();
    info.mediaUpdate(changed);
    CHECK(info.parcel()->media.url == "http://video.example.com/b.mp4");
    CHECK(info.version() != before);

    namespace c = parcel_media_command;
    CHECK_FALSE(info.mediaPlay().command.has_value());
    info.mediaCommand(1u << c::kPause, c::kPause, 0.0f);
    CHECK(info.mediaPlay().command == c::kPause);
    info.mediaCommand(1u << c::kTime, 0, 12.5f);
    CHECK(info.mediaPlay().command == c::kPause); // only the time said
    CHECK(info.mediaPlay().time == doctest::Approx(12.5f));
    info.mediaCommand(1u << c::kUnload, c::kUnload, 0.0f);
    CHECK(info.mediaPlay().command == c::kStop);
}

#ifndef _WIN32
#include "LoopbackHttpServer.hpp"

TEST_CASE("Music streams: over HTTP, with SHOUTcast metadata, to the speakers")
{
    // The tone as a station would send it: metadata every 1000 bytes.
    const auto mp3 = test::readTestFile("tone_440hz_half.mp3");
    REQUIRE(!mp3.empty());
    std::string body;
    std::string meta = "StreamTitle='Tester - Tone';";
    meta.resize(32, '\0');
    for(size_t at = 0; at < mp3.size(); at += 1000)
    {
        const size_t n = std::min<size_t>(1000, mp3.size() - at);
        body.append(reinterpret_cast<const char*>(mp3.data() + at), n);
        if(n == 1000)
        {
            body += static_cast<char>(2);
            body += meta;
        }
    }
    test::LoopbackHttpServer server(
        [&](const test::LoopbackRequest& r) -> std::optional<test::LoopbackReply>
        {
            if(r.path != "/radio")
            {
                return std::nullopt;
            }
            return test::LoopbackReply{200, body,
                                       "Content-Type: audio/mpeg\r\nicy-metaint: 1000\r\n"
                                       "icy-name: Test FM\r\nicy-genre: Tones\r\nicy-br: 64\r\n"};
        });
    auto http = net::createHttpClient();
    // Through the real output if there is one (silently), else just the
    // stream.
    auto engine = audio::AudioEngine::create();
    MusicStream music(*http, engine.get());
    music.setVolume(0.0f);
    music.play(server.url("/radio"));
    bool played = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(std::chrono::steady_clock::now() < deadline)
    {
        music.update();
        if(engine)
        {
            engine->update();
        }
        played |= music.status().state == MusicStream::State::Playing;
        if(!music.status().title.empty() && (played || !engine))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const MusicStream::Status& s = music.status();
    CHECK(s.station == "Test FM");
    CHECK(s.genre == "Tones");
    CHECK(s.bitrate == 64);
    CHECK(s.title == "Tester - Tone");
    CHECK(s.format == audio::AudioStream::Format::Mp3);
    CHECK(s.sampleRate == 44100);
    if(engine)
    {
        CHECK(played);
    }
    music.stop();
    CHECK(music.status().state == MusicStream::State::Stopped);
}

TEST_CASE("Music streams: a station that isn't there fails, saying why")
{
    test::LoopbackHttpServer server;
    auto http = net::createHttpClient();
    MusicStream music(*http, nullptr);
    music.play(server.url("/nothing-here"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(std::chrono::steady_clock::now() < deadline &&
          music.status().state != MusicStream::State::Failed)
    {
        music.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(music.status().state == MusicStream::State::Failed);
    CHECK(music.status().error == "HTTP 404");
}
#endif
