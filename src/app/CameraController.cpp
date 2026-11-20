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
}

void CameraController::setFirstPerson(bool on)
{
    firstPerson_ = on;
    rotating_ = false;
    panning_ = false;
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
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if(rotating_)
        {
            yaw_ -= event.motion.xrel * 0.3f;
            pitch_ = std::clamp(pitch_ + event.motion.yrel * 0.3f, -85.0f, 85.0f);
        }
        else if(panning_)
        {
            const glm::vec3 f = forward();
            const glm::vec3 right = glm::normalize(glm::cross(f, glm::vec3(0.0f, 0.0f, 1.0f)));
            const glm::vec3 up = glm::cross(right, f);
            const float scale = distance_ * 0.002f;
            target_ += (-right * event.motion.xrel + up * event.motion.yrel) * scale;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if(walking_)
        {
            break;
        }
        // All the way in, and in again: first person.
        if(event.wheel.y > 0.0f && distance_ <= 1.0f)
        {
            modeRequest_ = true;
        }
        distance_ = std::clamp(distance_ * std::pow(0.9f, event.wheel.y), 1.0f, 600.0f);
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
