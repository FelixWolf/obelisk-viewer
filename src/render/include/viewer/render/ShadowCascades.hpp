#pragma once

// The sun's shadow cascades for a frame: see computeCascades.

#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace viewer::render
{

struct Cascades
{
    glm::mat4 lightRotation{1.0f};       // world -> light view (looking away from the light)
    std::array<glm::mat4, 4> matrices{}; // world -> each cascade's clip space
    std::array<glm::vec4, 4> bounds{};   // light view: xy, z of the center; w radius
    glm::vec4 splits{0.0f};              // each slice's far end, depth along the view
    glm::vec4 texels{0.0f};              // texel size, meters
};

// Casters this far towards the light beyond a slice still cast into it.
inline constexpr float kCasterReach = 300.0f;
// Casters smaller than this many texels (radius) are left out of a cascade.
inline constexpr float kMinCasterTexels = 4.0f;

// Cascaded shadow maps for the sun: the view, out to the shadow distance,
// cut into slices that grow with distance (a mix of even and logarithmic
// splits); each slice gets an orthographic view from the light that covers
// a sphere around it. The sphere's size doesn't change as the camera
// turns, and its center snaps to whole texels, so shadow edges don't
// shimmer as the camera moves.
Cascades computeCascades(const Camera& camera, float aspect, const Shadows& shadows,
                         uint32_t mapSize);

// Whether a sphere (light view coordinates) can cast into a cascade.
bool castsInto(const glm::vec3& p, float r, const glm::vec4& bounds);

} // namespace viewer::render
