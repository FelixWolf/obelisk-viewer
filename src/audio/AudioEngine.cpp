#include "viewer/audio/AudioEngine.hpp"

#include "viewer/core/Log.hpp"

#include <miniaudio.h>

#include <cfloat>
#include <utility>

namespace viewer::audio
{

struct AudioEngine::Impl
{
    ma_engine engine{};
};

struct AudioEngine::VoiceData
{
    std::shared_ptr<const SoundClip> clip;
    ma_audio_buffer_ref buffer{};
    ma_sound sound{};
    bool bufferReady = false;
    bool soundReady = false;

    ~VoiceData()
    {
        if(soundReady)
        {
            ma_sound_uninit(&sound);
        }
        if(bufferReady)
        {
            ma_audio_buffer_ref_uninit(&buffer);
        }
    }
};

std::unique_ptr<AudioEngine> AudioEngine::create()
{
    auto impl = std::make_unique<Impl>();
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if(ma_engine_init(&config, &impl->engine) != MA_SUCCESS)
    {
        log::warn("audio: no output device");
        return nullptr;
    }
    log::info("audio: {} Hz, {} channels", ma_engine_get_sample_rate(&impl->engine),
              ma_engine_get_channels(&impl->engine));
    return std::unique_ptr<AudioEngine>(new AudioEngine(std::move(impl)));
}

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
    // Z up, as the scene.
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 0.0f, 1.0f);
}

AudioEngine::~AudioEngine()
{
    voices_.clear(); // before the engine
    ma_engine_uninit(&impl_->engine);
}

AudioEngine::VoiceData* AudioEngine::find(Voice voice) const
{
    const auto it = voices_.find(voice);
    return it == voices_.end() ? nullptr : it->second.get();
}

AudioEngine::Voice AudioEngine::play(std::shared_ptr<const SoundClip> clip,
                                     const PlayParams& params)
{
    if(!clip || clip->frames() == 0 || voices_.size() >= kMaxVoices)
    {
        return 0;
    }
    auto v = std::make_unique<VoiceData>();
    v->clip = std::move(clip);
    if(ma_audio_buffer_ref_init(ma_format_f32, v->clip->channels, v->clip->samples.data(),
                                v->clip->frames(), &v->buffer) != MA_SUCCESS)
    {
        return 0;
    }
    v->bufferReady = true;
    v->buffer.sampleRate = v->clip->sampleRate; // (not set by init in 0.11)
    const ma_uint32 flags = params.positional ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if(ma_sound_init_from_data_source(&impl_->engine, &v->buffer, flags, nullptr, &v->sound) !=
       MA_SUCCESS)
    {
        return 0;
    }
    v->soundReady = true;
    ma_sound& s = v->sound;
    if(params.positional)
    {
        ma_sound_set_attenuation_model(&s, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&s, params.referenceDistance);
        ma_sound_set_max_distance(&s, FLT_MAX);
        ma_sound_set_rolloff(&s, params.rolloff);
        ma_sound_set_doppler_factor(&s, 0.0f);
        ma_sound_set_position(&s, params.position.x, params.position.y, params.position.z);
    }
    ma_sound_set_volume(&s, params.gain);
    ma_sound_set_looping(&s, params.loop ? MA_TRUE : MA_FALSE);
    if(ma_sound_start(&s) != MA_SUCCESS)
    {
        return 0;
    }
    const Voice id = next_++;
    voices_.emplace(id, std::move(v));
    return id;
}

void AudioEngine::setPosition(Voice voice, const glm::vec3& position)
{
    if(VoiceData* v = find(voice))
    {
        ma_sound_set_position(&v->sound, position.x, position.y, position.z);
    }
}

void AudioEngine::setGain(Voice voice, float gain)
{
    if(VoiceData* v = find(voice))
    {
        ma_sound_set_volume(&v->sound, gain);
    }
}

void AudioEngine::setLooping(Voice voice, bool loop)
{
    if(VoiceData* v = find(voice))
    {
        ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
    }
}

void AudioEngine::setRolloff(Voice voice, float rolloff)
{
    if(VoiceData* v = find(voice))
    {
        ma_sound_set_rolloff(&v->sound, rolloff);
    }
}

void AudioEngine::stop(Voice voice)
{
    voices_.erase(voice);
}

bool AudioEngine::playing(Voice voice) const
{
    const VoiceData* v = find(voice);
    return v && !ma_sound_at_end(&v->sound) && ma_sound_is_playing(&v->sound);
}

float AudioEngine::progress(Voice voice) const
{
    VoiceData* v = find(voice); // (miniaudio's getters aren't const)
    if(!v)
    {
        return 1.0f;
    }
    ma_uint64 cursor = 0;
    if(ma_sound_get_cursor_in_pcm_frames(&v->sound, &cursor) != MA_SUCCESS ||
       v->clip->frames() == 0)
    {
        return 0.0f;
    }
    return static_cast<float>(static_cast<double>(cursor) / static_cast<double>(v->clip->frames()));
}

void AudioEngine::setListener(const glm::vec3& position, const glm::vec3& forward,
                              const glm::vec3& up)
{
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::setVolume(float volume)
{
    ma_engine_set_volume(&impl_->engine, volume);
}

void AudioEngine::update()
{
    std::erase_if(voices_, [](const auto& entry)
                  { return ma_sound_at_end(&entry.second->sound) != MA_FALSE; });
}

} // namespace viewer::audio
