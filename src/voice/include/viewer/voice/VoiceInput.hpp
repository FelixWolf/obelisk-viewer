#pragma once

// What we say: the microphone's sound, its echo of what the viewer plays
// and its noise taken out and its level evened (SpeexDSP), encoded (Opus, 20
// ms at a time) and sent to each connection, on a thread of its own. Muted,
// silence is sent, as the official viewer does.
//
// The microphone is opened the first time it's wanted, and stays open
// (unheard while muted, so talking starts at once and the echo's path stays
// known) until release().

#include "viewer/audio/AudioEngine.hpp"
#include "viewer/voice/Peer.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace viewer::voice
{

class VoiceInput
{
public:
    // `engine` (may be null): whose output to take out; outlives this.
    explicit VoiceInput(audio::AudioEngine* engine);
    ~VoiceInput();
    VoiceInput(const VoiceInput&) = delete;
    VoiceInput& operator=(const VoiceInput&) = delete;

    struct Sink
    {
        std::shared_ptr<Peer> peer;
        bool live = false; // gets the microphone (else silence)
        bool operator==(const Sink&) const = default;
    };
    void setSinks(std::vector<Sink> sinks);

    void setTalking(bool talking);
    bool talking() const { return talking_.load(std::memory_order_relaxed); }
    // Closes the microphone.
    void release();
    void setDevice(const std::string& name);
    struct Processing
    {
        bool echo = true;
        bool noise = true;
        bool level = true;
    };
    void setProcessing(const Processing& processing);

    // Our voice's loudness (0..1, about), as sent.
    float power() const { return power_.load(std::memory_order_relaxed); }
    bool open() const { return open_.load(std::memory_order_relaxed); }
    // It was wanted and couldn't be opened.
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

private:
    void run(const std::stop_token& stop);

    audio::AudioEngine* engine_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<Sink> sinks_;
    std::string device_;
    uint64_t deviceVersion_ = 0;
    Processing processing_;
    std::atomic<bool> talking_{false};
    std::atomic<bool> wanted_{false};
    std::atomic<float> power_{0.0f};
    std::atomic<bool> open_{false};
    std::atomic<bool> failed_{false};
    std::jthread thread_; // last
};

} // namespace viewer::voice
