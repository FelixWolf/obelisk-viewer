#include "viewer/audio/SoundClip.hpp"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstring>

namespace viewer::audio
{

namespace
{

// Reading the file from memory (libvorbisfile's callbacks).
struct Memory
{
    std::span<const uint8_t> data;
    size_t position = 0;
};

size_t readMemory(void* out, size_t size, size_t count, void* source)
{
    auto* m = static_cast<Memory*>(source);
    const size_t wanted = size * count;
    const size_t n = std::min(wanted, m->data.size() - m->position);
    std::memcpy(out, m->data.data() + m->position, n);
    m->position += n;
    return size == 0 ? 0 : n / size;
}

int seekMemory(void* source, ogg_int64_t offset, int whence)
{
    auto* m = static_cast<Memory*>(source);
    const auto size = static_cast<ogg_int64_t>(m->data.size());
    ogg_int64_t to = whence == SEEK_SET   ? offset
                     : whence == SEEK_CUR ? static_cast<ogg_int64_t>(m->position) + offset
                                          : size + offset;
    if(to < 0 || to > size)
    {
        return -1;
    }
    m->position = static_cast<size_t>(to);
    return 0;
}

long tellMemory(void* source)
{
    return static_cast<long>(static_cast<Memory*>(source)->position);
}

// Longer than any Second Life sound (ten seconds), with room to spare.
constexpr uint64_t kMaxSeconds = 120;

} // namespace

std::optional<SoundClip> decodeVorbis(std::span<const uint8_t> data)
{
    if(data.empty())
    {
        return std::nullopt;
    }
    Memory memory{data, 0};
    const ov_callbacks callbacks{readMemory, seekMemory, nullptr, tellMemory};
    OggVorbis_File file;
    if(ov_open_callbacks(&memory, &file, nullptr, 0, callbacks) != 0)
    {
        return std::nullopt;
    }
    std::optional<SoundClip> result;
    const vorbis_info* info = ov_info(&file, -1);
    if(info && info->channels >= 1 && info->channels <= 2 && info->rate > 0)
    {
        SoundClip clip;
        clip.channels = static_cast<uint32_t>(info->channels);
        clip.sampleRate = static_cast<uint32_t>(info->rate);
        const uint64_t most = kMaxSeconds * clip.sampleRate;
        int section = 0;
        for(;;)
        {
            float** pcm = nullptr;
            const long frames = ov_read_float(&file, &pcm, 4096, &section);
            if(frames < 0)
            {
                continue; // a hole in the data: skip it
            }
            if(frames == 0 || clip.frames() >= most)
            {
                break;
            }
            // Planar to interleaved.
            const size_t at = clip.samples.size();
            clip.samples.resize(at + static_cast<size_t>(frames) * clip.channels);
            for(long f = 0; f < frames; f++)
            {
                for(uint32_t c = 0; c < clip.channels; c++)
                {
                    clip.samples[at + static_cast<size_t>(f) * clip.channels + c] =
                        std::clamp(pcm[c][f], -1.0f, 1.0f);
                }
            }
        }
        if(clip.frames() > 0)
        {
            result = std::move(clip);
        }
    }
    ov_clear(&file);
    return result;
}

} // namespace viewer::audio
