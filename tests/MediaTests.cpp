#include "TestUtil.hpp"

#include "viewer/audio/AudioStream.hpp"

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
