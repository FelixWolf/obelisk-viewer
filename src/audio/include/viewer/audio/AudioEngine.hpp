#pragma once

// Sound output: miniaudio's engine (mixing, resampling, 3D positioning)
// behind a small interface. Voices play clips, positioned in the scene or
// not (interface sounds), at a gain, looping or not. Distance attenuation is
// the official viewer's (OpenAL's inverse distance, clamped: full within the
// reference distance, then reference / (reference + rolloff (d - reference))).
// All calls from one thread (the main one); miniaudio mixes on its own.

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
    };
    // Starts playing; 0 if it can't (too many voices, or a bad clip). The
    // clip is kept while the voice plays.
    Voice play(std::shared_ptr<const SoundClip> clip, const PlayParams& params);
    void setPosition(Voice voice, const glm::vec3& position);
    void setGain(Voice voice, float gain);
    void setLooping(Voice voice, bool loop);
    void setRolloff(Voice voice, float rolloff);
    void stop(Voice voice);
    // Still playing (false once it ended or was stopped).
    bool playing(Voice voice) const;
    // Where in the clip it is, 0..1.
    float progress(Voice voice) const;

    // The listener: where, facing which way, which way is up.
    void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    // Everything's volume (0..1).
    void setVolume(float volume);

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
