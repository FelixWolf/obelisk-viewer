#include "viewer/render/ShadowCascades.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace viewer::render
{

Cascades computeCascades(const Camera& camera, float aspect, const Shadows& shadows,
                         uint32_t mapSize)
{
    Cascades c;
    const glm::vec3 towardLight = glm::normalize(shadows.towardLight);
    const glm::vec3 lightUp =
        std::abs(towardLight.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    c.lightRotation = glm::lookAt(glm::vec3(0.0f), -towardLight, lightUp);

    const float nearEnd = camera.nearPlane;
    const float farEnd = std::max(nearEnd * 2.0f, std::min(shadows.distance, camera.farPlane));
    const glm::vec3 forward = glm::normalize(camera.target - camera.position);
    const glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
    const glm::vec3 up = glm::cross(right, forward);
    const float tanY = std::tan(camera.verticalFov * 0.5f);
    const float tanX = tanY * aspect;

    float previous = nearEnd;
    for(size_t i = 0; i < 4; i++)
    {
        const float t = static_cast<float>(i + 1) / 4.0f;
        const float split = glm::mix(nearEnd + (farEnd - nearEnd) * t,
                                     nearEnd * std::pow(farEnd / nearEnd, t), 0.85f);
        c.splits[static_cast<glm::length_t>(i)] = split;

        std::array<glm::vec3, 8> corners;
        size_t k = 0;
        for(const float d : {previous, split})
        {
            for(const float sx : {-1.0f, 1.0f})
            {
                for(const float sy : {-1.0f, 1.0f})
                {
                    corners[k++] = camera.position + forward * d + right * (sx * tanX * d) +
                                   up * (sy * tanY * d);
                }
            }
        }
        glm::vec3 center(0.0f);
        for(const glm::vec3& corner : corners)
        {
            center += corner / 8.0f;
        }
        float radius = 0.0f;
        for(const glm::vec3& corner : corners)
        {
            radius = std::max(radius, glm::length(corner - center));
        }
        radius = std::ceil(radius * 2.0f) / 2.0f;
        const float texel = 2.0f * radius / static_cast<float>(mapSize);

        glm::vec3 inLight(c.lightRotation * glm::vec4(center, 1.0f));
        inLight.x = std::floor(inLight.x / texel) * texel;
        inLight.y = std::floor(inLight.y / texel) * texel;
        // The light view looks along -Z: distances are -z.
        const glm::mat4 ortho =
            glm::ortho(inLight.x - radius, inLight.x + radius, inLight.y - radius,
                       inLight.y + radius, -inLight.z - radius - kCasterReach, -inLight.z + radius);
        c.matrices[i] = ortho * c.lightRotation;
        c.bounds[i] = glm::vec4(inLight, radius);
        c.texels[static_cast<glm::length_t>(i)] = texel;
        previous = split;
    }
    return c;
}

// Whether a sphere (light view coordinates) can cast into a cascade.
bool castsInto(const glm::vec3& p, float r, const glm::vec4& bounds)
{
    return std::abs(p.x - bounds.x) <= bounds.w + r && std::abs(p.y - bounds.y) <= bounds.w + r &&
           p.z >= bounds.z - bounds.w - r && p.z <= bounds.z + bounds.w + kCasterReach + r;
}

} // namespace viewer::render
