#include "viewer/audio/AudioEngine.hpp"

#include "viewer/core/Log.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <span>
#include <utility>

namespace viewer::audio
{

// Wind: noise made on the audio thread, steered from the main one.
struct AudioEngine::Wind
{
    ma_data_source_base base{}; // first: miniaudio sees this as the data source
    uint32_t sampleRate = 48000;
    std::atomic<float> targetGain{0.0f};
    std::atomic<float> targetFrequency{100.0f};
    std::atomic<float> targetPan{0.5f};
    // Audio thread only.
    uint32_t random = 0x12345678u;
    float pink[3] = {0.0f, 0.0f, 0.0f};
    float y1 = 0.0f;
    float y2 = 0.0f;
    float gain = 0.0f;
    float frequency = 100.0f;
    float pan = 0.5f;
    float last = 0.0f;
    float step = 0.0f;
    int sub = 0;

    float noise()
    {
        // xorshift32, centered on 0 (-0.5..0.5).
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return static_cast<float>(random) / 4294967296.0f - 0.5f;
    }

    // One synthesized sample (made at half the output rate, then eased
    // between): pink noise through a resonant two-pole filter at the
    // frequency, at the gain.
    float synthesize()
    {
        const float white = noise();
        pink[0] = 0.99765f * pink[0] + white * 0.0990460f;
        pink[1] = 0.96300f * pink[1] + white * 0.2965164f;
        pink[2] = 0.57000f * pink[2] + white * 1.0526913f;
        const float x = pink[0] + pink[1] + pink[2] + white * 0.1848f;

        const float period = 2.0f / static_cast<float>(sampleRate);
        const float target = targetFrequency.load(std::memory_order_relaxed);
        frequency = std::abs(target - frequency) < frequency * 0.112f
                        ? target
                        : 0.999f * frequency + 0.001f * target;
        // Pole radius squared from a 50 Hz bandwidth; the gain keeps the
        // peak near unity.
        constexpr float kTwoPi = 6.28318530718f;
        const float r2 = std::exp(-kTwoPi * 50.0f * period);
        const float b1 = -4.0f * r2 / (1.0f + r2) * std::cos(kTwoPi * frequency * period);
        const float a0 = (1.0f - r2) * std::sqrt(std::max(0.0f, 1.0f - b1 * b1 / (4.0f * r2)));
        const float y = a0 * x - b1 * y1 - r2 * y2;
        y2 = y1;
        y1 = y;

        gain = 0.999f * gain + 0.001f * targetGain.load(std::memory_order_relaxed);
        pan = 0.999f * pan + 0.001f * targetPan.load(std::memory_order_relaxed);
        return y * gain;
    }

    void read(float* out, ma_uint64 frames)
    {
        for(ma_uint64 i = 0; i < frames; i++)
        {
            if(sub == 0)
            {
                step = (synthesize() - last) * 0.5f;
                sub = 2;
            }
            last += step;
            sub--;
            const float right = last * pan;
            out[2 * i] = last - right;
            out[2 * i + 1] = right;
        }
    }
};

namespace
{

ma_result windRead(ma_data_source* source, void* out, ma_uint64 frames, ma_uint64* read)
{
    static_cast<AudioEngine::Wind*>(source)->read(static_cast<float*>(out), frames);
    if(read)
    {
        *read = frames;
    }
    return MA_SUCCESS;
}

ma_result windSeek(ma_data_source*, ma_uint64)
{
    return MA_SUCCESS;
}

ma_result windFormat(ma_data_source* source, ma_format* format, ma_uint32* channels,
                     ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap)
{
    *format = ma_format_f32;
    *channels = 2;
    *sampleRate = static_cast<AudioEngine::Wind*>(source)->sampleRate;
    if(channelMap && channelMapCap >= 2)
    {
        channelMap[0] = MA_CHANNEL_FRONT_LEFT;
        channelMap[1] = MA_CHANNEL_FRONT_RIGHT;
    }
    return MA_SUCCESS;
}

ma_data_source_vtable kWindSource = {windRead, windSeek, windFormat, nullptr, nullptr, nullptr, 0};

} // namespace

// A stream, read on the audio thread.
struct AudioEngine::StreamSource
{
    ma_data_source_base base{}; // first: miniaudio sees this as the data source
    std::shared_ptr<PcmSource> stream;
    uint32_t sampleRate = 0;
};

namespace
{

ma_result streamRead(ma_data_source* source, void* out, ma_uint64 frames, ma_uint64* read)
{
    const auto* s = static_cast<AudioEngine::StreamSource*>(source);
    const uint64_t got = s->stream->read(static_cast<float*>(out), frames);
    if(read)
    {
        *read = got;
    }
    return got == 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result streamFormat(ma_data_source* source, ma_format* format, ma_uint32* channels,
                       ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap)
{
    *format = ma_format_f32;
    *channels = 2;
    *sampleRate = static_cast<AudioEngine::StreamSource*>(source)->sampleRate;
    if(channelMap && channelMapCap >= 2)
    {
        channelMap[0] = MA_CHANNEL_FRONT_LEFT;
        channelMap[1] = MA_CHANNEL_FRONT_RIGHT;
    }
    return MA_SUCCESS;
}

ma_data_source_vtable kStreamSource = {streamRead, windSeek, streamFormat, nullptr, nullptr,
                                       nullptr,    0};

} // namespace

struct AudioEngine::Impl
{
    ma_engine engine{};
    std::unique_ptr<Wind> wind;
    ma_sound windSound{};
    bool windReady = false;
    std::atomic<bool> tapped{false};
    FrameRing tap{kSampleRate / 2};

    // The audio thread's: what was just mixed.
    void mixed(const float* frames, ma_uint64 count, ma_uint32 channels)
    {
        float mono[256];
        for(ma_uint64 at = 0; at < count;)
        {
            const auto n = static_cast<size_t>(std::min<ma_uint64>(count - at, std::size(mono)));
            for(size_t i = 0; i < n; i++)
            {
                float sum = 0.0f;
                for(ma_uint32 c = 0; c < channels; c++)
                {
                    sum += frames[(at + i) * channels + c];
                }
                mono[i] = sum / static_cast<float>(channels);
            }
            tap.write(std::span(mono, n));
            at += n;
        }
    }
};

struct AudioEngine::VoiceData
{
    std::shared_ptr<const SoundClip> clip;
    ma_audio_buffer_ref buffer{};
    std::unique_ptr<StreamSource> stream; // or this
    ma_sound sound{};
    bool bufferReady = false;
    bool streamReady = false;
    bool soundReady = false;
    bool fading = false; // stopping once faded out

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
        if(streamReady)
        {
            ma_data_source_uninit(&stream->base);
        }
    }
};

std::unique_ptr<AudioEngine> AudioEngine::create()
{
    auto impl = std::make_unique<Impl>();
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    config.sampleRate = kSampleRate; // (converted to the device's)
    config.pProcessUserData = impl.get();
    config.onProcess = [](void* user, float* frames, ma_uint64 count)
    {
        auto* i = static_cast<Impl*>(user);
        if(i->tapped.load(std::memory_order_relaxed))
        {
            i->mixed(frames, count, ma_engine_get_channels(&i->engine));
        }
    };
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
    if(impl_->windReady)
    {
        ma_sound_uninit(&impl_->windSound);
        ma_data_source_uninit(&impl_->wind->base);
    }
    ma_engine_uninit(&impl_->engine);
}

void AudioEngine::setWind(float gain, float frequency, float panRight)
{
    if(!impl_->windReady)
    {
        // Made the first time it's wanted.
        auto wind = std::make_unique<Wind>();
        wind->sampleRate = ma_engine_get_sample_rate(&impl_->engine);
        ma_data_source_config config = ma_data_source_config_init();
        config.vtable = &kWindSource;
        if(ma_data_source_init(&config, &wind->base) != MA_SUCCESS)
        {
            return;
        }
        if(ma_sound_init_from_data_source(&impl_->engine, &wind->base,
                                          MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                          &impl_->windSound) != MA_SUCCESS)
        {
            ma_data_source_uninit(&wind->base);
            return;
        }
        impl_->wind = std::move(wind);
        impl_->windReady = true;
        ma_sound_start(&impl_->windSound);
    }
    Wind& w = *impl_->wind;
    w.targetGain.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
    w.targetFrequency.store(std::clamp(frequency, 20.0f, 4000.0f), std::memory_order_relaxed);
    w.targetPan.store(std::clamp(panRight, 0.0f, 1.0f), std::memory_order_relaxed);
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
    if(params.fadeInSeconds > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&s, 0.0f, 1.0f,
                                          static_cast<ma_uint64>(params.fadeInSeconds * 1000.0f));
    }
    if(ma_sound_start(&s) != MA_SUCCESS)
    {
        return 0;
    }
    const Voice id = next_++;
    voices_.emplace(id, std::move(v));
    return id;
}

AudioEngine::Voice AudioEngine::playStream(std::shared_ptr<PcmSource> stream,
                                           const PlayParams& params)
{
    if(!stream || stream->sampleRate() == 0)
    {
        return 0;
    }
    auto v = std::make_unique<VoiceData>();
    v->stream = std::make_unique<StreamSource>();
    v->stream->stream = std::move(stream);
    v->stream->sampleRate = v->stream->stream->sampleRate();
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &kStreamSource;
    if(ma_data_source_init(&config, &v->stream->base) != MA_SUCCESS)
    {
        return 0;
    }
    v->streamReady = true;
    if(ma_sound_init_from_data_source(&impl_->engine, &v->stream->base,
                                      MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                      &v->sound) != MA_SUCCESS)
    {
        return 0;
    }
    v->soundReady = true;
    ma_sound_set_volume(&v->sound, params.gain);
    if(params.fadeInSeconds > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&v->sound, 0.0f, 1.0f,
                                          static_cast<ma_uint64>(params.fadeInSeconds * 1000.0f));
    }
    if(ma_sound_start(&v->sound) != MA_SUCCESS)
    {
        return 0;
    }
    const Voice id = next_++;
    voices_.emplace(id, std::move(v));
    return id;
}

void AudioEngine::fadeOut(Voice voice, float seconds)
{
    VoiceData* v = find(voice);
    if(!v)
    {
        return;
    }
    if(seconds <= 0.0f)
    {
        stop(voice);
        return;
    }
    ma_sound_stop_with_fade_in_milliseconds(&v->sound, static_cast<ma_uint64>(seconds * 1000.0f));
    v->fading = true;
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
    if(!v->clip)
    {
        return 0.0f; // a stream: no end to measure against
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

void AudioEngine::setTapped(bool tapped)
{
    impl_->tapped.store(tapped, std::memory_order_relaxed);
}

FrameRing& AudioEngine::tap()
{
    return impl_->tap;
}

void AudioEngine::update()
{
    std::erase_if(voices_,
                  [](const auto& entry)
                  {
                      ma_sound* s = &entry.second->sound;
                      return ma_sound_at_end(s) != MA_FALSE ||
                             (entry.second->fading && ma_sound_is_playing(s) == MA_FALSE);
                  });
}

} // namespace viewer::audio
