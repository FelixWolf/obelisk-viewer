#pragma once

#include "viewer/image/Image.hpp"

#include <cstdint>

namespace viewer::image
{

// Converts planar decoder output (one int plane per component, any bit depth,
// signed or unsigned) to an 8-bit interleaved Image. Decoder backends share it
// so they all produce identical output from identical samples.
//
// `getSample(component, x, y)` returns the raw sample as int32_t.
// `precision[c]` and `isSigned[c]` describe component c.
template <typename GetSample>
void interleaveTo8Bit(Image& out, uint32_t width, uint32_t height, uint32_t components,
                      const uint8_t* precision, const bool* isSigned, GetSample&& getSample)
{
    out.width = width;
    out.height = height;
    out.components = components;
    out.pixels.resize(static_cast<size_t>(width) * height * components);

    for(uint32_t c = 0; c < components; c++)
    {
        const int shift = static_cast<int>(precision[c]) - 8;
        const int32_t bias = isSigned[c] ? (1 << (precision[c] - 1)) : 0;
        uint8_t* dst = out.pixels.data() + c;
        for(uint32_t y = 0; y < height; y++)
        {
            for(uint32_t x = 0; x < width; x++)
            {
                int32_t v = getSample(c, x, y) + bias;
                v = shift > 0 ? (v >> shift) : (v << -shift);
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                *dst = static_cast<uint8_t>(v);
                dst += components;
            }
        }
    }
}

} // namespace viewer::image
