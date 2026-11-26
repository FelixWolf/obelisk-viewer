#pragma once

// Moves our avatar: W/S and the up/down arrows walk forward and back, A/D
// and left/right turn (with Shift, step sideways), E / Page Up and Q, C /
// Page Down go up and down, Home toggles flying. The simulator does the moving; we send it control flags and the
// direction the body faces (Session::setControls).

#include "viewer/protocol/MessageTypes.hpp"

#include <SDL3/SDL_events.h>

#include <glm/glm.hpp>

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
    // First person: W/S (and up/down) walk, A/D (and left/right) step
    // sideways; the body faces where the camera does (the app sets yaw).
    void setFirstPerson(bool on) { firstPerson_ = on; }
    // The left button in first person: held, then let go (as the official
    // viewer's LLToolCompGun, for scripts that take controls).
    void setMouselookButton(bool down)
    {
        mouselookUp_ = mouselookDown_ && !down;
        mouselookDown_ = down;
    }

    // A controller's stick (first person): x right, y forward, -1..1.
    void setStick(glm::vec2 stick) { stick_ = stick; }

    bool flying() const { return flying_; }
    void setFlying(bool on) { flying_ = on; }
    // Stands up from what we sit on (the stand up flag, sent for a moment).
    void standUp() { standUpFor_ = 0.3f; }

    uint32_t controlFlags() const { return flags_; }
    protocol::Quaternion bodyRotation() const;

private:
    float yaw_ = 0.0f;
    bool flying_ = false;
    float standUpFor_ = 0.0f; // seconds the stand up flag is still sent
    bool autoWalk_ = false;
    bool firstPerson_ = false;
    bool mouselookDown_ = false;
    bool mouselookUp_ = false;
    uint32_t flags_ = 0;
    glm::vec2 stick_{0.0f};
};

} // namespace viewer
