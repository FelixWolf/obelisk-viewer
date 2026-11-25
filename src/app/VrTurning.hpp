#pragma once

// Turning with a controller's stick: in steps, one a flick, or smoothly
// while it's held.

#include <algorithm>
#include <cmath>

namespace viewer
{

class VrTurning
{
public:
    bool smooth = false;
    float snapAngle = 30.0f; // degrees a flick
    float speed = 90.0f;     // degrees a second, the stick all the way

    // The turn for a frame (degrees, counter-clockwise), `stick` how far
    // it's pushed right (-1..1) and `seconds` since the last frame.
    float update(float stick, float seconds)
    {
        // A flick: pushed past most of the way, then let back. Kept track of
        // while smooth too, so switching with it held doesn't turn a step.
        const float pushed = std::abs(stick);
        const bool flicked = !held_ && pushed > 0.7f;
        held_ = pushed > (held_ ? 0.3f : 0.7f);
        if(!smooth)
        {
            return flicked ? -std::copysign(snapAngle, stick) : 0.0f;
        }
        // From nothing at the dead zone's edge. A long frame (a hitch)
        // doesn't turn far.
        constexpr float kDeadZone = 0.2f;
        const float rate = std::clamp((pushed - kDeadZone) / (1.0f - kDeadZone), 0.0f, 1.0f);
        return -std::copysign(rate * speed * std::clamp(seconds, 0.0f, 0.1f), stick);
    }

private:
    bool held_ = false;
};

} // namespace viewer
