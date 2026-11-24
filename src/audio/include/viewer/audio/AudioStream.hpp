#pragma once

// An internet audio stream (a parcel's music, later media's sound): its
// bytes go in as they arrive, from any thread; a thread of its own decodes
// them (MP3 through miniaudio, Ogg Vorbis through libvorbis, chained
// streams too) into a few seconds of stereo samples, which the engine
// plays (AudioEngine::playStream) once there's enough. Running dry, it's
// silent until there's enough again. AAC (some SHOUTcast stations) isn't
// decoded: the stream fails, saying so.
//
// The bytes are the audio alone: a SHOUTcast/Icecast stream's metadata is
// taken out before (the app does, it knows the headers).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace viewer::audio
{

class AudioStream
{
public:
    enum class Format : uint8_t
    {
        Unknown, // not enough bytes to tell
        Mp3,
        Vorbis,
        Aac,   // recognized, not decoded
        Other, // not audio we know
    };
    // From the Content-Type (if it says) or the first bytes.
    static Format sniff(std::string_view contentType, std::span<const uint8_t> start);
    static std::string_view formatName(Format format);

    // `contentType`: the response's, if any. Starts its thread.
    explicit AudioStream(std::string contentType = {});
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    // More of it (any thread). Past a few megabytes not decoded yet, the
    // oldest are dropped.
    void push(std::span<const uint8_t> bytes);
    // No more will come (the connection ended).
    void finish();

    enum class State : uint8_t
    {
        Buffering, // not enough decoded yet (or again)
        Playing,
        Ended, // all of it played (after finish())
        Failed,
    };
    State state() const;
    // Why it failed.
    std::string error() const;
    Format format() const { return format_.load(std::memory_order_acquire); }
    // The output's, once decoding started (0 until then): what
    // playStream() can begin with.
    uint32_t sampleRate() const { return ready_.load(std::memory_order_acquire) ? rate_ : 0; }
    // The source's channels (output is always stereo).
    uint32_t channels() const { return channels_.load(std::memory_order_relaxed); }
    float bufferedSeconds() const;
    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // The audio thread's: `frames` stereo frames into `out`, silence where
    // there's nothing to play yet (buffering); how many it wrote, fewer
    // only at the end (0 once it ended).
    uint64_t read(float* out, uint64_t frames);

    // How much it plays once it starts and after running dry, and how
    // much it holds.
    static constexpr float kPrebufferSeconds = 1.5f;
    static constexpr float kBufferSeconds = 8.0f;
    static constexpr size_t kMaxPendingBytes = 4u << 20;

    struct Decoding; // (the decoder thread's; opaque)

private:
    void run(std::stop_token stop);
    void fail(std::string why);
    // The decoder thread's: up to `n` bytes, waiting for them; 0 once they
    // ended (or it's stopping).
    size_t pull(uint8_t* out, size_t n);
    // ... and samples out: interleaved, at `rate`, `channels` of them.
    bool write(const float* samples, uint64_t frames, uint32_t channels, uint32_t rate);
    bool writeStereo(const float* stereo, uint64_t frames);
    void decodeMp3();
    void decodeVorbis();

    std::string contentType_;
    std::atomic<Format> format_{Format::Unknown};

    // Bytes in, not decoded yet.
    mutable std::mutex mutex_;
    std::condition_variable_any arrived_;
    std::vector<uint8_t> pending_;
    size_t pendingAt_ = 0; // read up to here
    bool finished_ = false;
    std::string error_;

    // Decoded: a ring of stereo frames at rate_ (both set before ready_).
    struct Ring;
    std::unique_ptr<Ring> ring_;
    uint32_t rate_ = 0;
    uint32_t prebufferFrames_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<uint32_t> channels_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> decoded_{false}; // the decoder's done (ended or failed)
    std::atomic<bool> buffering_{true};
    std::atomic<bool> drained_{false};
    std::atomic<uint32_t> underruns_{0};
    std::unique_ptr<Decoding> decoding_;

    std::jthread thread_; // last
};

} // namespace viewer::audio
