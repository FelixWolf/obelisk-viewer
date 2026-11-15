#include "viewer/render/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace viewer::render
{

glm::mat4 Camera::view() const
{
    return glm::lookAt(position, target, up);
}

glm::vec4 Camera::tangents(float aspect) const
{
    if(edges)
    {
        return *edges;
    }
    const float y = std::tan(verticalFov * 0.5f);
    const float x = y * aspect;
    return glm::vec4(-x, x, -y, y);
}

glm::mat4 Camera::projection(float aspect) const
{
    const glm::vec4 t = tangents(aspect);
    return glm::frustum(t.x * nearPlane, t.y * nearPlane, t.z * nearPlane, t.w * nearPlane,
                        nearPlane, farPlane);
}

} // namespace viewer::render
