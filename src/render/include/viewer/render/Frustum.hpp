#pragma once

// The volume a camera sees, as six planes, for culling: anything entirely
// outside isn't drawn. Built from a view-projection matrix with 0..1 depth
// (the project's convention); for VR, one volume that holds both eyes'.

#include <glm/glm.hpp>

#include <array>

namespace viewer::render
{

struct Camera;

class Frustum
{
public:
    // Everything is inside (no culling).
    Frustum() = default;
    static Frustum fromViewProjection(const glm::mat4& viewProjection);
    // The camera's, for a viewport `aspect` (width / height) wide.
    static Frustum fromCamera(const Camera& camera, float aspect);

    bool containsSphere(const glm::vec3& center, float radius) const;
    bool containsBox(const glm::vec3& min, const glm::vec3& max) const;
    // Whether a sphere's shadow can fall inside: the sphere swept without
    // end away from a light in direction `towardLight`.
    bool containsShadowOf(const glm::vec3& center, float radius,
                          const glm::vec3& towardLight) const;

private:
    // Normals point inwards and are unit length: inside where
    // dot(plane.xyz, p) + plane.w >= 0 for all six.
    std::array<glm::vec4, 6> planes_{
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)};
};

} // namespace viewer::render
