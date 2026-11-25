#pragma once

// Orbit camera: right-drag rotates around the target, the wheel zooms,
// middle-drag pans, WASD moves the target, Q/E lowers and raises it.
//
// First person (the official viewer's mouselook): the camera at the eyes
// (setEye), turned by the mouse's motion with no button held; the keys
// move the avatar instead. The wheel all the way in asks for first person,
// out of it for third (takeModeRequest): the app switches.
//
// Walking (the login lobby, bodiless): the camera at the target, at eye
// height; right-drag looks around, WASD walks on the ground (no up or
// down), the wheel does nothing.

#include "viewer/render/Scene.hpp"

#include <SDL3/SDL_events.h>

#include <cmath>
#include <optional>
#include <utility>

namespace viewer
{

class CameraController
{
public:
    void handleEvent(const SDL_Event& event);
    // Turning or panning with the mouse (it isn't pointing at anything then).
    bool dragging() const { return rotating_ || panning_; }
    // Applies held keys; call once per frame.
    void update(float seconds);

    void lookAt(glm::vec3 target, float distance, float yawDegrees, float pitchDegrees);
    render::Camera camera() const;
    glm::vec3 target() const { return target_; }
    // Moves the camera without turning it (following the avatar, or the
    // world origin moving).
    void shift(glm::vec3 offset) { target_ += offset; }
    float yaw() const { return yaw_; }
    // Turns it (degrees, counter-clockwise): a headset's right stick.
    void turn(float degrees) { yaw_ = std::remainder(yaw_ + degrees, 360.0f); }
    float pitch() const { return pitch_; }

    void setFirstPerson(bool on);
    bool firstPerson() const { return firstPerson_; }
    void setWalking(bool on);
    bool walking() const { return walking_; }
    void setEye(glm::vec3 eye) { eye_ = eye; }
    // What the wheel asked for since last time: first person (true),
    // third (false), or nothing.
    std::optional<bool> takeModeRequest() { return std::exchange(modeRequest_, std::nullopt); }

private:
    glm::vec3 forward() const;

    glm::vec3 target_{0.0f};
    float distance_ = 12.0f;
    float yaw_ = 90.0f; // degrees counter-clockwise from east; 90 looks north (+Y)
    float pitch_ = 25.0f;
    bool rotating_ = false;
    bool panning_ = false;
    bool firstPerson_ = false;
    bool walking_ = false;
    glm::vec3 eye_{0.0f};
    std::optional<bool> modeRequest_;
};

} // namespace viewer
