#pragma once

#include <cstdint>
#include <vector>

namespace viewer::image
{

// Decoded pixels as the rest of the viewer consumes them: 8 bits per channel,
// channels interleaved (e.g. RGBARGBA...), rows tightly packed, top row first.
struct Image
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t components = 0;     // 1 = luminance, 2 = luminance+alpha, 3 = RGB, 4 = RGBA
    std::vector<uint8_t> pixels; // width * height * components bytes
    // A fifth component, kept apart (width * height bytes, top row first;
    // empty if there's none): avatar bakes carry their morph mask in it
    // (llimageworker.cpp decodes it as the "aux" channel 4).
    std::vector<uint8_t> aux;

    size_t rowBytes() const { return static_cast<size_t>(width) * components; }
};

} // namespace viewer::image
