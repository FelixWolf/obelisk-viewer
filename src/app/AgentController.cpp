#include "AgentController.hpp"

#include "viewer/session/Session.hpp"

#include <SDL3/SDL_keyboard.h>

#include <cmath>

namespace viewer
{

namespace
{

constexpr float kTurnDegreesPerSecond = 120.0f;

} // namespace

void AgentController::handleEvent(const SDL_Event& event)
{
    if(event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
       event.key.scancode == SDL_SCANCODE_HOME)
    {
        flying_ = !flying_;
    }
}

void AgentController::update(float seconds, bool keyboardFree)
{
    namespace control = session::control;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    auto held = [&](SDL_Scancode key) { return keyboardFree && keys[key]; };
    const bool shift = held(SDL_SCANCODE_LSHIFT) || held(SDL_SCANCODE_RSHIFT);

    uint32_t flags = flying_ ? control::kFly : 0;
    if(held(SDL_SCANCODE_UP) || autoWalk_)
    {
        flags |= control::kAtPos | control::kFastAt;
    }
    if(held(SDL_SCANCODE_DOWN))
    {
        flags |= control::kAtNeg | control::kFastAt;
    }
    const bool left = held(SDL_SCANCODE_LEFT);
    const bool right = held(SDL_SCANCODE_RIGHT);
    if(shift)
    {
        flags |= left ? control::kLeftPos | control::kFastLeft : 0;
        flags |= right ? control::kLeftNeg | control::kFastLeft : 0;
    }
    else if(left != right)
    {
        // We turn the body ourselves; the flag plays the turning animation.
        flags |= left ? control::kYawPos : control::kYawNeg;
        yaw_ += (left ? 1.0f : -1.0f) * kTurnDegreesPerSecond * seconds;
        yaw_ = std::remainder(yaw_, 360.0f);
    }
    if(held(SDL_SCANCODE_PAGEUP))
    {
        flags |= control::kUpPos | control::kFastUp;
    }
    if(held(SDL_SCANCODE_PAGEDOWN))
    {
        flags |= control::kUpNeg | control::kFastUp;
    }
    flags_ = flags;
}

protocol::Quaternion AgentController::bodyRotation() const
{
    // About +Z. Rounded so tiny changes don't count as new rotations.
    const float half = std::round(yaw_ * 10.0f) / 10.0f * 3.14159265f / 360.0f;
    return {0.0f, 0.0f, std::sin(half), std::cos(half)};
}

} // namespace viewer
