#pragma once

// An internet radio stream playing (a parcel's music): fetched over HTTP
// as it plays, decoded (audio::AudioStream), heard as is, not positioned.
// Asks for SHOUTcast/Icecast metadata (Icy-MetaData: 1) and takes it out
// of the audio: the station's name and genre from the headers, the song
// playing from the stream (StreamTitle). Switching fades out the old and in
// the new (as llvieweraudio.cpp, half a second each). A connection that
// drops is made again, a few times, as long as it played before.
//
// One stream at a time; the app says what to play (the parcel's URL, or
// nothing).

#include "viewer/audio/AudioEngine.hpp"
#include "viewer/audio/AudioStream.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace viewer
{

// Takes SHOUTcast metadata out of a stream: every `interval` bytes of
// audio, a length byte (times 16) and that much text
// ("StreamTitle='...';StreamUrl='...';"). Interval 0: there's none.
class IcyReader
{
public:
    explicit IcyReader(size_t interval) : interval_(interval), untilMeta_(interval) {}
    // Gives the audio among these bytes to `audio`, keeps the metadata.
    void feed(std::span<const uint8_t> bytes,
              const std::function<void(std::span<const uint8_t>)>& audio);
    // The song from the latest metadata, once (nothing: none since).
    std::optional<std::string> takeTitle();

private:
    size_t interval_;
    size_t untilMeta_;
    size_t metaLeft_ = 0;
    bool inMeta_ = false;
    std::string meta_;
    std::optional<std::string> title_;
};

// StreamTitle's value in a metadata block (nothing: not there), as UTF-8
// (stations sending Latin-1 are converted).
std::optional<std::string> icyStreamTitle(std::string_view metadata);
// Text as UTF-8: as is if it is already, otherwise read as Latin-1.
std::string toUtf8(std::string_view text);

class MusicStream
{
public:
    // `engine` may be null (no output): then nothing plays (the stream is
    // still fetched and its details known, but it never leaves Buffering).
    MusicStream(net::HttpClient& http, audio::AudioEngine* engine);
    ~MusicStream();
    MusicStream(const MusicStream&) = delete;
    MusicStream& operator=(const MusicStream&) = delete;

    // Plays this (from the start, even if it's what plays); empty stops.
    void play(const std::string& url);
    void stop() { play({}); }
    // 0..1.
    void setVolume(float volume);
    float volume() const { return volume_; }
    // Once a frame.
    void update();

    enum class State : uint8_t
    {
        Stopped,
        Connecting,
        Buffering,
        Playing,
        Failed,
    };
    static std::string_view stateName(State state);
    struct Status
    {
        std::string url;
        State state = State::Stopped;
        std::string error; // Failed: why
        std::string station;
        std::string genre;
        std::string title; // the song, if the station says
        audio::AudioStream::Format format = audio::AudioStream::Format::Unknown;
        uint32_t sampleRate = 0;
        uint32_t channels = 0;
        int bitrate = 0; // kbps, if the station says
    };
    const Status& status() const { return status_; }
    // Bumped when the status changes (for the UI).
    uint64_t revision() const { return revision_; }

    // How long the fades take, how many times a dropped stream is
    // reconnected, and how long between.
    static constexpr float kFadeSeconds = 0.5f;
    static constexpr int kMaxReconnects = 5;
    static constexpr auto kReconnectDelay = std::chrono::seconds(3);

private:
    // One HTTP connection: the I/O thread fills it, the main thread reads.
    struct Connection;
    void connect();
    void disconnect(bool fade);
    void set(State state, std::string error = {});

    net::HttpClient& http_;
    audio::AudioEngine* engine_;
    std::unique_ptr<net::HttpRequestGroup> group_;
    std::shared_ptr<Connection> connection_;
    net::HttpRequestId request_ = 0;
    audio::AudioEngine::Voice voice_ = 0;
    Status status_;
    float volume_ = 0.5f;
    bool everPlayed_ = false; // this URL, on any connection
    int reconnects_ = 0;
    std::optional<std::chrono::steady_clock::time_point> reconnectAt_;
    uint64_t revision_ = 0;
};

} // namespace viewer
