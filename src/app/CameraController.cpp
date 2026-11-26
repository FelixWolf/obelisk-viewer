#include "CameraController.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace viewer
{

void CameraController::lookAt(glm::vec3 target, float distance, float yawDegrees,
                              float pitchDegrees)
{
    target_ = target;
    distance_ = distance;
    yaw_ = yawDegrees;
    pitch_ = pitchDegrees;
    offAvatar_ = false;
    followDistance_ = distance;
    glide_.reset();
}

void CameraController::glideTo(glm::vec3 target, float distance, float yawDegrees,
                               float pitchDegrees)
{
    if(firstPerson_ || walking_)
    {
        lookAt(target, distance, yawDegrees, pitchDegrees);
        return;
    }
    offAvatar_ = false;
    followDistance_ = distance;
    glide_ = Glide{target, distance, yawDegrees, pitchDegrees};
}

void CameraController::animate(float seconds)
{
    if(!glide_)
    {
        return;
    }
    if(firstPerson_ || walking_ || dragging())
    {
        glide_.reset(); // where it is now, then
        return;
    }
    // Where it ends, as a camera position and the point it looks at.
    const float yaw = glm::radians(glide_->yaw);
    const float pitch = glm::radians(glide_->pitch);
    const glm::vec3 endForward(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                               -std::sin(pitch));
    const glm::vec3 endPosition = glide_->target - endForward * glide_->distance;
    const glm::vec3 position = camera().position;
    // Both slide there, eased: most of the way in about half a second.
    const float blend = 1.0f - std::exp(-seconds * 6.0f);
    const glm::vec3 nextTarget = glm::mix(target_, glide_->target, blend);
    const glm::vec3 nextPosition = glm::mix(position, endPosition, blend);
    const glm::vec3 d = nextTarget - nextPosition;
    const float length = glm::length(d);
    if(length < 0.05f || (glm::length(endPosition - nextPosition) < 0.01f &&
                          glm::length(glide_->target - nextTarget) < 0.01f))
    {
        const Glide end = *glide_;
        lookAt(end.target, end.distance, end.yaw, end.pitch);
        return;
    }
    target_ = nextTarget;
    distance_ = length;
    yaw_ = glm::degrees(std::atan2(d.y, d.x));
    pitch_ = std::clamp(glm::degrees(-std::asin(d.z / length)), -89.0f, 89.0f);
}

void CameraController::focusOn(glm::vec3 point)
{
    const glm::vec3 position = camera().position;
    const glm::vec3 d = point - position;
    const float length = glm::length(d);
    if(firstPerson_ || walking_ || length < 0.05f)
    {
        return;
    }
    target_ = point;
    distance_ = length;
    yaw_ = glm::degrees(std::atan2(d.y, d.x));
    pitch_ = std::clamp(glm::degrees(-std::asin(d.z / length)), -89.0f, 89.0f);
    offAvatar_ = true;
    glide_.reset();
}

void CameraController::pan(float dx, float dy)
{
    // Across the view, as far as the pointer moves at the target's distance.
    const glm::vec3 f = forward();
    const glm::vec3 right = glm::normalize(glm::cross(f, glm::vec3(0.0f, 0.0f, 1.0f)));
    const glm::vec3 up = glm::cross(right, f);
    const float scale = distance_ * 0.002f;
    target_ += (-right * dx + up * dy) * scale;
}

glm::vec3 CameraController::forward() const
{
    // Yaw counter-clockwise from east (+X); positive pitch looks down.
    const float yaw = glm::radians(yaw_);
    const float pitch = glm::radians(pitch_);
    return glm::vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                     -std::sin(pitch));
}

render::Camera CameraController::camera() const
{
    render::Camera camera;
    if(firstPerson_)
    {
        // From the eyes, near enough not to cut through hands held close.
        camera.position = eye_;
        camera.target = eye_ + forward();
        camera.nearPlane = 0.05f;
        return camera;
    }
    if(walking_)
    {
        camera.position = target_;
        camera.target = target_ + forward();
        return camera;
    }
    camera.target = target_;
    camera.position = target_ - forward() * distance_;
    return camera;
}

void CameraController::setWalking(bool on)
{
    walking_ = on;
    rotating_ = false;
    panning_ = false;
    altDragging_ = false;
}

void CameraController::setFirstPerson(bool on)
{
    firstPerson_ = on;
    rotating_ = false;
    panning_ = false;
    altDragging_ = false;
    if(on)
    {
        pitch_ = 0.0f; // level, the way the avatar faces (set by the app)
    }
}

void CameraController::handleEvent(const SDL_Event& event)
{
    if(firstPerson_)
    {
        // The mouse turns the view; the wheel out goes back to third person.
        if(event.type == SDL_EVENT_MOUSE_MOTION)
        {
            yaw_ = std::remainder(yaw_ - event.motion.xrel * 0.15f, 360.0f);
            pitch_ = std::clamp(pitch_ + event.motion.yrel * 0.15f, -85.0f, 85.0f);
        }
        else if(event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y < 0.0f)
        {
            modeRequest_ = false;
        }
        return;
    }
    switch(event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if(event.button.button == SDL_BUTTON_RIGHT)
        {
            rotating_ = down;
        }
        else if(event.button.button == SDL_BUTTON_MIDDLE && !walking_)
        {
            panning_ = down;
        }
        else if(event.button.button == SDL_BUTTON_LEFT && !walking_)
        {
            altDragging_ = down && (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        // Panned or Alt-moved: off the avatar. Turning with the right
        // button isn't: it's looking about, and the avatar walks that way.
        if((panning_ || altDragging_) && !walking_ &&
           (event.motion.xrel != 0.0f || event.motion.yrel != 0.0f))
        {
            offAvatar_ = true;
        }
        if(altDragging_)
        {
            const SDL_Keymod mods = SDL_GetModState();
            const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
            const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
            if(ctrl && shift)
            {
                pan(event.motion.xrel, event.motion.yrel);
            }
            else if(ctrl)
            {
                yaw_ = std::remainder(yaw_ - event.motion.xrel * 0.3f, 360.0f);
                pitch_ = std::clamp(pitch_ + event.motion.yrel * 0.3f, -85.0f, 85.0f);
            }
            else
            {
                // Up: closer. Left and right: around it, level.
                distance_ =
                    std::clamp(distance_ * std::exp(event.motion.yrel * 0.01f), 0.25f, 600.0f);
                yaw_ = std::remainder(yaw_ - event.motion.xrel * 0.3f, 360.0f);
            }
        }
        else if(rotating_)
        {
            yaw_ -= event.motion.xrel * 0.3f;
            pitch_ = std::clamp(pitch_ + event.motion.yrel * 0.3f, -85.0f, 85.0f);
        }
        else if(panning_)
        {
            pan(event.motion.xrel, event.motion.yrel);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if(walking_)
        {
            break;
        }
        // All the way in, and in again: first person.
        if(event.wheel.y > 0.0f && distance_ <= 1.0f && !glide_)
        {
            modeRequest_ = true;
        }
        if(glide_)
        {
            // Gliding back: the zoom is where it ends.
            glide_->distance =
                std::clamp(glide_->distance * std::pow(0.9f, event.wheel.y), 1.0f, 600.0f);
            followDistance_ = glide_->distance;
            break;
        }
        // Not out to 1 m at once from closer in (an Alt zoom goes nearer).
        distance_ = std::clamp(distance_ * std::pow(0.9f, event.wheel.y),
                               std::min(distance_, 1.0f), 600.0f);
        if(!offAvatar_)
        {
            followDistance_ = distance_;
        }
        break;
    default:
        break;
    }
}

void CameraController::update(float seconds)
{
    if(firstPerson_)
    {
        return; // the keys move the avatar
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    glm::vec3 flat = forward();
    flat.z = 0.0f;
    if(glm::length(flat) < 1e-4f)
    {
        return;
    }
    flat = glm::normalize(flat);
    const glm::vec3 right = glm::cross(flat, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::vec3 move{0.0f};
    move += keys[SDL_SCANCODE_W] ? flat : glm::vec3(0.0f);
    move -= keys[SDL_SCANCODE_S] ? flat : glm::vec3(0.0f);
    move += keys[SDL_SCANCODE_D] ? right : glm::vec3(0.0f);
    move -= keys[SDL_SCANCODE_A] ? right : glm::vec3(0.0f);
    if(walking_)
    {
        // A walk (a run with shift), on the ground.
        const float length = glm::length(move);
        if(length > 0.0f)
        {
            target_ += move / length * (keys[SDL_SCANCODE_LSHIFT] ? 4.0f : 1.6f) * seconds;
        }
        return;
    }
    move.z += keys[SDL_SCANCODE_E] ? 1.0f : 0.0f;
    move.z -= keys[SDL_SCANCODE_Q] ? 1.0f : 0.0f;
    const float speed = std::max(5.0f, distance_) * (keys[SDL_SCANCODE_LSHIFT] ? 3.0f : 1.0f);
    target_ += move * speed * seconds;
}

} // namespace viewer
