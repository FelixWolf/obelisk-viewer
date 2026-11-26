#pragma once

// The viewer's end of the media host: starts the program, says Init, and
// from then on sends what it's given and hands back what the host says.
// A thread of its own reads the host; tabs' sound goes from there straight
// into each tab's TabAudio (played by the audio engine), everything else
// waits for take() on the main thread.
//
// An interface, so the app's media code can be tested without the program.

#include "viewer/audio/PcmSource.hpp"
#include "viewer/media/Protocol.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace viewer::media
{

class ChildProcess;

// A tab's sound: what the host sends, played as it comes. Stereo out (mono
// is doubled); silent while there's too little to play smoothly. Ends (read
// gives 0) once the tab closed.
class TabAudio final : public audio::PcmSource
{
public:
    TabAudio(uint32_t sampleRate, uint32_t channels);
    uint32_t sampleRate() const override { return rate_; }
    uint64_t read(float* out, uint64_t frames) override;
    // The reader's.
    void push(std::span<const float> samples);
    void end();
    float bufferedSeconds() const;

    // Played once this much is in, and again after running dry; more than
    // kMaxSeconds is dropped from the oldest (the host never runs slow, so
    // that's only a stall of ours).
    static constexpr float kPrebufferSeconds = 0.1f;
    static constexpr float kMaxSeconds = 0.5f;

private:
    const uint32_t rate_;
    const uint32_t channels_;
    mutable std::mutex mutex_;
    std::vector<float> ring_; // stereo frames
    size_t head_ = 0;         // next written, in frames
    size_t size_ = 0;         // frames in it
    bool playing_ = false;
    bool ended_ = false;
};

class MediaHost
{
public:
    enum class State : uint8_t
    {
        Starting,
        Ready,
        Failed, // never started, said Failed, or went away
    };
    virtual ~MediaHost() = default;
    virtual State state() const = 0;
    // Why it failed (once Failed).
    virtual std::string error() const = 0;
    // False if it can't be sent (gone).
    virtual bool send(const Message& message) = 0;
    // What the host said since (not tabs' sound: audio()).
    virtual void take(std::vector<Message>& out) = 0;
    // A tab's sound while it has some (since AudioStart); null otherwise.
    virtual std::shared_ptr<TabAudio> audio(TabId tab) = 0;
};

// The real one: the program at `program`, started now.
std::unique_ptr<MediaHost> startMediaHost(const std::filesystem::path& program, Init init);

} // namespace viewer::media
