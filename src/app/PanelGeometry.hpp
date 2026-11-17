#pragma once

// Where things go on a flat panel (render::Panel) and where a ray meets one:
// the VR controls' menus and keyboard are pointed at with rays.

#include <glm/glm.hpp>

#include <optional>

namespace viewer
{

// A panel's frame at `position` facing `toward` (its +Z towards it), kept
// upright (its +Y as near the scene's +Z as it can be).
inline glm::mat4 panelFacing(const glm::vec3& position, const glm::vec3& toward)
{
    glm::vec3 z = toward - position;
    z = glm::length(z) > 1e-5f ? glm::normalize(z) : glm::vec3(-1.0f, 0.0f, 0.0f);
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if(std::abs(glm::dot(z, up)) > 0.99f)
    {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    const glm::vec3 x = glm::normalize(glm::cross(up, z));
    const glm::vec3 y = glm::cross(z, x);
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(x, 0.0f);
    frame[1] = glm::vec4(y, 0.0f);
    frame[2] = glm::vec4(z, 0.0f);
    frame[3] = glm::vec4(position, 1.0f);
    return frame;
}

// Where a ray meets a panel's front: the panel's pixel (y down from its top
// left), and how far along the ray. Nothing if it misses the plane, comes
// from behind it, or (unless `anywhere`) is outside the panel.
struct PanelHit
{
    glm::vec2 pixel{0.0f};
    float distance = 0.0f;
    glm::vec3 position{0.0f}; // in the scene
};

inline std::optional<PanelHit> panelHit(const glm::mat4& frame, glm::vec2 size,
                                        float metersPerPixel, const glm::vec3& origin,
                                        const glm::vec3& direction, bool anywhere = false)
{
    const glm::vec3 normal(frame[2]);
    const glm::vec3 center(frame[3]);
    const float facing = glm::dot(direction, normal);
    if(facing >= -1e-5f)
    {
        return std::nullopt; // parallel, or from behind
    }
    const float distance = glm::dot(center - origin, normal) / facing;
    if(distance < 0.0f)
    {
        return std::nullopt;
    }
    const glm::vec3 at = origin + direction * distance;
    const glm::vec3 local = at - center;
    const glm::vec2 meters(glm::dot(local, glm::vec3(frame[0])),
                           glm::dot(local, glm::vec3(frame[1])));
    const glm::vec2 pixel(meters.x / metersPerPixel + size.x * 0.5f,
                          size.y * 0.5f - meters.y / metersPerPixel);
    if(!anywhere && (pixel.x < 0.0f || pixel.y < 0.0f || pixel.x > size.x || pixel.y > size.y))
    {
        return std::nullopt;
    }
    return PanelHit{pixel, distance, at};
}

} // namespace viewer
