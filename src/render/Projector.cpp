#include "viewer/render/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace viewer::render
{

LocalLight::Projector projectorFor(const glm::vec3& position, const glm::quat& rotation,
                                   const glm::vec3& scale, float range, float fov, float focus,
                                   float ambiance)
{
    LocalLight::Projector p;
    p.axis = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    // The near plane is the prim's -Z face; the apex sits behind it, so
    // that `fov` spans the prim's Y size there.
    fov = std::clamp(fov, 1e-3f, 3.0f);
    const float nearDistance = std::max(scale.y, 1e-3f) * 0.5f / std::tan(fov * 0.5f);
    p.origin = position + p.axis * (scale.z * 0.5f) - p.axis * nearDistance;
    const float farDistance = std::max(range + nearDistance - scale.z, nearDistance + 0.01f);
    p.depth = farDistance - nearDistance;
    p.focus = focus;
    p.ambiance = ambiance;

    const glm::mat4 toLight =
        glm::inverse(glm::translate(glm::mat4(1.0f), p.origin) * glm::mat4_cast(rotation));
    const float aspect = std::max(scale.x, 1e-3f) / std::max(scale.y, 1e-3f);
    // Clip space's -1..1 to texture space's 0..1 (z too: >= 0 is in front).
    const glm::mat4 bias = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f)) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
    p.matrix = bias * glm::perspectiveRH_NO(fov, aspect, nearDistance, farDistance) * toLight;
    return p;
}

} // namespace viewer::render
