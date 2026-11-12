#include "viewer/render/Frustum.hpp"

#include "viewer/render/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace viewer::render
{

Frustum Frustum::fromViewProjection(const glm::mat4& m)
{
    // Rows of the matrix combined (Gribb and Hartmann); with 0..1 depth the
    // near plane is the third row alone.
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
    Frustum f;
    f.planes_ = {row3 + row0, row3 - row0, row3 + row1, row3 - row1, row2, row3 - row2};
    for(glm::vec4& p : f.planes_)
    {
        p /= glm::length(glm::vec3(p));
    }
    return f;
}

Frustum Frustum::fromCamera(const Camera& camera, float aspect)
{
    const glm::mat4 view = glm::lookAt(camera.position, camera.target, camera.up);
    const glm::mat4 projection =
        glm::perspective(camera.verticalFov, aspect, camera.nearPlane, camera.farPlane);
    return fromViewProjection(projection * view);
}

bool Frustum::containsSphere(const glm::vec3& center, float radius) const
{
    for(const glm::vec4& p : planes_)
    {
        if(glm::dot(glm::vec3(p), center) + p.w < -radius)
        {
            return false;
        }
    }
    return true;
}

bool Frustum::containsBox(const glm::vec3& min, const glm::vec3& max) const
{
    for(const glm::vec4& p : planes_)
    {
        // The corner furthest along the plane's normal.
        const glm::vec3 corner(p.x >= 0.0f ? max.x : min.x, p.y >= 0.0f ? max.y : min.y,
                               p.z >= 0.0f ? max.z : min.z);
        if(glm::dot(glm::vec3(p), corner) + p.w < 0.0f)
        {
            return false;
        }
    }
    return true;
}

} // namespace viewer::render
