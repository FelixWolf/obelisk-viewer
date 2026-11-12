#pragma once

// Moves our avatar: the arrow keys walk forward and back and turn (with
// Shift, step sideways), Page Up / Page Down go up and down, Home toggles
// flying. The simulator does the moving; we send it control flags and the
// direction the body faces (Session::setControls).

#include "viewer/protocol/MessageTypes.hpp"

#include <SDL3/SDL_events.h>

#include <cstdint>

namespace viewer
{

class AgentController
{
public:
    void handleEvent(const SDL_Event& event);
    // Reads held keys (unless `keyboardFree` is false) and turns the body.
    void update(float seconds, bool keyboardFree);

    // Where the body faces, degrees counter-clockwise from east.
    void setYaw(float degrees) { yaw_ = degrees; }
    float yaw() const { return yaw_; }
    // Keeps walking forward without keys (for testing crossings).
    void setAutoWalk(bool walk) { autoWalk_ = walk; }

    uint32_t controlFlags() const { return flags_; }
    protocol::Quaternion bodyRotation() const;

private:
    float yaw_ = 0.0f;
    bool flying_ = false;
    bool autoWalk_ = false;
    uint32_t flags_ = 0;
};

} // namespace viewer
