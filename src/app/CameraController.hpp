#pragma once

// Orbit camera: right-drag rotates around the target, the wheel zooms,
// middle-drag pans. With no avatar of ours to move, WASD moves the target
// and Q/E lowers and raises it (update); with one, the keys are the
// avatar's (AgentController).
//
// Alt and the left button, as the official viewer's (lltoolfocus.cpp): the
// click focuses on what it's on (focusOn, the camera staying where it is);
// dragging then zooms toward it (up and down) and goes around it (left and
// right); with Ctrl too, orbits it; with Ctrl and Shift too, pans across the
// view. The modifiers are read as the mouse moves, so they can change
// mid-drag.
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
    bool dragging() const { return rotating_ || panning_ || altDragging_; }
    // The left button went down with Alt (handleEvent): the press is the
    // camera's, not a touch.
    bool altDragging() const { return altDragging_; }
    // Turning with the right button.
    bool turning() const { return rotating_; }
    // Looks at `point` from where the camera is now (an Alt click).
    void focusOn(glm::vec3 point);
    // As lookAt, but getting there over a moment: the camera and the point
    // it looks at sliding there together, as the official viewer's camera
    // going back behind the avatar (LLAgentCamera's interpolation). Moving
    // the camera with the mouse (or lookAt) stops it where it is.
    void glideTo(glm::vec3 target, float distance, float yawDegrees, float pitchDegrees);
    bool gliding() const { return glide_.has_value(); }
    // Advances a glide; call once per frame.
    void animate(float seconds);
    // Lets go of any drag (the window losing the mouse: its release may
    // never come).
    void stopDragging()
    {
        rotating_ = false;
        panning_ = false;
        altDragging_ = false;
    }
    // Moved off the view behind the avatar with the mouse (panned, an Alt
    // focus or drag; not right-drag turning) until lookAt or glideTo:
    // moving the avatar puts it back (the app), as the official viewer's
    // camera resetting on movement.
    bool offAvatar() const { return offAvatar_; }
    // How far behind the avatar it sits when following it: lookAt's, and the
    // wheel's while following (not an Alt zoom's).
    float followDistance() const { return followDistance_; }
    // Applies held keys; call once per frame.
    void update(float seconds);

    void lookAt(glm::vec3 target, float distance, float yawDegrees, float pitchDegrees);
    render::Camera camera() const;
    glm::vec3 target() const { return target_; }
    // Moves the camera without turning it (following the avatar, or the
    // world origin moving).
    void shift(glm::vec3 offset)
    {
        target_ += offset;
        if(glide_)
        {
            glide_->target += offset;
        }
    }
    float yaw() const { return yaw_; }
    // Turns it (degrees, counter-clockwise): a headset's right stick.
    void turn(float degrees)
    {
        yaw_ = std::remainder(yaw_ + degrees, 360.0f);
        if(glide_)
        {
            glide_->yaw = std::remainder(glide_->yaw + degrees, 360.0f);
        }
    }
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
    void pan(float dx, float dy);

    // Where a glide ends.
    struct Glide
    {
        glm::vec3 target{0.0f};
        float distance = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    std::optional<Glide> glide_;

    glm::vec3 target_{0.0f};
    float distance_ = 12.0f;
    float yaw_ = 90.0f; // degrees counter-clockwise from east; 90 looks north (+Y)
    float pitch_ = 25.0f;
    bool rotating_ = false;
    bool panning_ = false;
    bool altDragging_ = false;
    bool offAvatar_ = false;
    float followDistance_ = 12.0f;
    bool firstPerson_ = false;
    bool walking_ = false;
    glm::vec3 eye_{0.0f};
    std::optional<bool> modeRequest_;
};

} // namespace viewer
