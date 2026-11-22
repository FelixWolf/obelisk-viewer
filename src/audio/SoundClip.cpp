#include "viewer/audio/SoundClip.hpp"

#include <cstdlib>
#include <limits>

extern "C"
{
    int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                                 short** output);
}

namespace viewer::audio
{

std::optional<SoundClip> decodeVorbis(std::span<const uint8_t> data)
{
    if(data.empty() || data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }
    int channels = 0;
    int rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(data.data(), static_cast<int>(data.size()),
                                                &channels, &rate, &pcm);
    if(frames <= 0 || !pcm || channels < 1 || channels > 2 || rate <= 0)
    {
        std::free(pcm);
        return std::nullopt;
    }
    SoundClip clip;
    clip.channels = static_cast<uint32_t>(channels);
    clip.sampleRate = static_cast<uint32_t>(rate);
    const size_t count = static_cast<size_t>(frames) * clip.channels;
    clip.samples.resize(count);
    for(size_t i = 0; i < count; i++)
    {
        clip.samples[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    std::free(pcm);
    return clip;
}

} // namespace viewer::audio
