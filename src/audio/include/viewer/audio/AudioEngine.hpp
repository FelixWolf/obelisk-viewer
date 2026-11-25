#pragma once

// Sound output: miniaudio's engine (mixing, resampling, 3D positioning)
// behind a small interface. Voices play clips, positioned in the scene or
// not (interface sounds), at a gain, looping or not. Distance attenuation is
// the official viewer's (OpenAL's inverse distance, clamped: full within the
// reference distance, then reference / (reference + rolloff (d - reference))).
// All calls from one thread (the main one); miniaudio mixes on its own.

#include "viewer/audio/FrameRing.hpp"
#include "viewer/audio/PcmSource.hpp"
#include "viewer/audio/SoundClip.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace viewer::audio
{

class AudioEngine
{
public:
    // Nothing if there's no audio output.
    static std::unique_ptr<AudioEngine> create();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    using Voice = uint64_t; // 0: none

    struct PlayParams
    {
        bool positional = true; // false: as is, wherever the listener is
        glm::vec3 position{0.0f};
        float gain = 1.0f;
        bool loop = false;
        float referenceDistance = 1.0f;
        float rolloff = 1.0f;
        float fadeInSeconds = 0.0f; // from silence to the gain
    };
    // Starts playing; 0 if it can't (too many voices, or a bad clip). The
    // clip is kept while the voice plays.
    Voice play(std::shared_ptr<const SoundClip> clip, const PlayParams& params);
    // Plays a stream (not positioned) from its decoded samples; 0 until it
    // has some (PcmSource::sampleRate()). Ends when the stream does. Streams
    // are few: kMaxVoices doesn't hold them back.
    Voice playStream(std::shared_ptr<PcmSource> stream, const PlayParams& params);
    void setPosition(Voice voice, const glm::vec3& position);
    void setGain(Voice voice, float gain);
    void setLooping(Voice voice, bool loop);
    void setRolloff(Voice voice, float rolloff);
    void stop(Voice voice);
    // Stops it, fading out over this long first.
    void fadeOut(Voice voice, float seconds);
    // Still playing (false once it ended or was stopped).
    bool playing(Voice voice) const;
    // Where in the clip it is, 0..1.
    float progress(Voice voice) const;

    // The listener: where, facing which way, which way is up.
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    // Everything's volume (0..1).
    void setVolume(float volume);

    // Wind noise around the listener (not positioned): filtered pink noise
    // whose loudness, pitch and left/right balance follow these (gain
    // 0..1, the filter's center frequency in Hz, right channel share
    // 0..1), easing towards them. Starts silent.
    void setWind(float gain, float frequency, float panRight);
    struct Wind;         // (the generator; opaque)
    struct StreamSource; // (a stream as miniaudio reads it; opaque)

    // What's played, mono at kSampleRate, while it's wanted (for taking the
    // echo out of the microphone's sound); one thread may read it.
    void setTapped(bool tapped);
    FrameRing& tap();
    static constexpr uint32_t kSampleRate = 48000;

    // Forgets voices that ended. Once a frame.
    void update();
    size_t voices() const { return voices_.size(); }

    static constexpr size_t kMaxVoices = 64;

private:
    struct Impl;
    struct VoiceData;
    explicit AudioEngine(std::unique_ptr<Impl> impl);
    VoiceData* find(Voice voice) const;

    std::unique_ptr<Impl> impl_;
    std::unordered_map<Voice, std::unique_ptr<VoiceData>> voices_;
    Voice next_ = 1;
};

} // namespace viewer::audio
