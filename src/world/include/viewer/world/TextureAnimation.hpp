#pragma once

// Texture animation (llSetTextureAnim), as the official viewer runs it
// (LLTextureAnim, LLViewerTextureAnim::animateTextures): flipbook frames
// from a grid, smooth scrolling, rotating or scaling, on one face or all.
// It stands in for the parts of a face's texture transform it animates.

#include <cstdint>
#include <optional>
#include <span>

namespace viewer::world
{

struct TextureAnimation
{
    // Mode bits.
    static constexpr uint8_t On = 0x01;
    static constexpr uint8_t Loop = 0x02;
    static constexpr uint8_t Reverse = 0x04;
    static constexpr uint8_t PingPong = 0x08;
    static constexpr uint8_t Smooth = 0x10;
    static constexpr uint8_t Rotate = 0x20;
    static constexpr uint8_t Scale = 0x40;

    uint8_t mode = 0;
    int8_t face = -1; // -1: every face
    uint8_t sizeX = 1;
    uint8_t sizeY = 1;
    float start = 0.0f;
    float length = 0.0f; // frames (0: the whole grid)
    float rate = 0.0f;   // frames a second
    bool operator==(const TextureAnimation&) const = default;
};

// The 16 bytes an update carries (mode, face, size x, y, then start, length
// and rate as little-endian floats); nothing for any other size. Full
// updates (`atLeastOneFrame`) count a grid of 0 as 1 unless smooth.
std::optional<TextureAnimation> decodeTextureAnimation(std::span<const uint8_t> data,
                                                       bool atLeastOneFrame);

// Where an animation is: what it sets (the rest of the face's transform
// stays) and to what.
struct AnimatedTransform
{
    bool rotates = false;
    bool scales = false;
    bool translates = false;
    float rotation = 0.0f; // radians
    float scaleS = 1.0f;
    float scaleT = 1.0f;
    float offsetS = 0.0f;
    float offsetT = 0.0f;
};

// The animation `frames` frames in (its rate times the time it has run);
// nothing if it's off.
std::optional<AnimatedTransform> animateTexture(const TextureAnimation& animation, float frames);

} // namespace viewer::world
