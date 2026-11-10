#pragma once

// Orbit camera: right-drag rotates around the target, the wheel zooms,
// middle-drag pans, WASD moves the target, Q/E lowers and raises it.

#include "viewer/render/Scene.hpp"

#include <SDL3/SDL_events.h>

namespace viewer
{

class CameraController
{
public:
    void handleEvent(const SDL_Event& event);
    // Applies held keys; call once per frame.
    void update(float seconds);

    void lookAt(glm::vec3 target, float distance, float yawDegrees, float pitchDegrees);
    render::Camera camera() const;
    glm::vec3 target() const { return target_; }

private:
    glm::vec3 forward() const;

    glm::vec3 target_{0.0f};
    float distance_ = 12.0f;
    float yaw_ = 90.0f; // degrees counter-clockwise from east; 90 looks north (+Y)
    float pitch_ = 25.0f;
    bool rotating_ = false;
    bool panning_ = false;
};

} // namespace viewer
