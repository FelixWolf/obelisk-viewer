#pragma once

// A decoded sound, ready to play: 32-bit float samples, channels
// interleaved. Second Life's sounds are Ogg Vorbis, mono or stereo, up to
// ten seconds (llvorbisencode.h).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace viewer::audio
{

struct SoundClip
{
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    std::vector<float> samples; // frames * channels

    uint64_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    double seconds() const
    {
        return sampleRate == 0 ? 0.0 : static_cast<double>(frames()) / sampleRate;
    }
    size_t byteSize() const { return samples.size() * sizeof(float); }
};

// Decodes an Ogg Vorbis file held in memory; nothing if it isn't one, has
// no audio, or has more than two channels.
std::optional<SoundClip> decodeVorbis(std::span<const uint8_t> data);

} // namespace viewer::audio
