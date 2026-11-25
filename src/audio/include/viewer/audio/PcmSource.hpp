#pragma once

// Samples made as they're played: a stream, people's voices.

#include <cstdint>

namespace viewer::audio
{

class PcmSource
{
public:
    virtual ~PcmSource() = default;
    // Its output's (always stereo); 0: not ready to play yet.
    virtual uint32_t sampleRate() const = 0;
    // The audio thread's: `frames` stereo frames into `out`; how many it
    // wrote, 0 once it ended.
    virtual uint64_t read(float* out, uint64_t frames) = 0;
};

} // namespace viewer::audio
