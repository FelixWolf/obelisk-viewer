#include "viewer/world/TextureAnimation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace viewer::world
{

std::optional<TextureAnimation> decodeTextureAnimation(std::span<const uint8_t> data,
                                                       bool atLeastOneFrame)
{
    if(data.size() != 16)
    {
        return std::nullopt;
    }
    TextureAnimation a;
    a.mode = data[0];
    a.face = static_cast<int8_t>(data[1]);
    a.sizeX = data[2];
    a.sizeY = data[3];
    if(atLeastOneFrame && !(a.mode & TextureAnimation::Smooth))
    {
        a.sizeX = std::max<uint8_t>(a.sizeX, 1);
        a.sizeY = std::max<uint8_t>(a.sizeY, 1);
    }
    std::memcpy(&a.start, data.data() + 4, 4);
    std::memcpy(&a.length, data.data() + 8, 4);
    std::memcpy(&a.rate, data.data() + 12, 4);
    return a;
}

std::optional<AnimatedTransform> animateTexture(const TextureAnimation& a, float frames)
{
    using A = TextureAnimation;
    if(!(a.mode & A::On))
    {
        return std::nullopt;
    }
    const bool smooth = (a.mode & A::Smooth) != 0;
    const float count =
        a.length != 0.0f
            ? a.length
            : std::max(1.0f, static_cast<float>(a.sizeX) * static_cast<float>(a.sizeY));
    float full = count;
    if(a.mode & A::PingPong)
    {
        full = smooth               ? 2.0f * count
               : (a.mode & A::Loop) ? std::max(1.0f, 2.0f * count - 2.0f)
                                    : std::max(1.0f, 2.0f * count - 1.0f);
    }

    float frame = frames;
    if(a.mode & A::Loop)
    {
        frame = std::fmod(frame, full);
    }
    else
    {
        frame = std::min(full - 1.0f, frame);
    }
    if(!smooth)
    {
        frame = std::min(full - 1.0f, std::floor(frame + 0.01f));
    }
    if((a.mode & A::PingPong) && frame >= count)
    {
        frame = smooth ? count - (frame - count) : (count - 1.99f) - (frame - count);
    }
    if(a.mode & A::Reverse)
    {
        frame = smooth ? count - frame : (count - 0.99f) - frame;
    }
    frame += a.start;
    if(!smooth)
    {
        frame = std::round(frame);
    }

    AnimatedTransform t;
    if(a.mode & A::Rotate)
    {
        t.rotates = true;
        t.rotation = frame;
    }
    else if(a.mode & A::Scale)
    {
        t.scales = true;
        t.scaleS = t.scaleT = frame;
    }
    else
    {
        t.translates = true;
        if(a.sizeX && a.sizeY)
        {
            t.scales = true;
            t.scaleS = 1.0f / static_cast<float>(a.sizeX);
            t.scaleT = 1.0f / static_cast<float>(a.sizeY);
            const float column = std::fmod(frame, static_cast<float>(a.sizeX));
            const float row = std::trunc(frame / static_cast<float>(a.sizeX));
            t.offsetS = (-0.5f + 0.5f * t.scaleS) + column * t.scaleS;
            t.offsetT = (0.5f - 0.5f * t.scaleT) - row * t.scaleT;
        }
        else
        {
            t.offsetS = frame;
        }
    }
    return t;
}

} // namespace viewer::world
