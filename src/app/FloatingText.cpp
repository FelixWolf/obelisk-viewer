#include "FloatingText.hpp"

#include <glm/glm.hpp>

namespace viewer
{

namespace
{

// Fully shown this near (meters from the camera, once brought nearer), gone
// this much further on (LLHUDText's fade distance and range).
constexpr float kFadeDistance = 8.0f;
constexpr float kFadeRange = 4.0f;

} // namespace

void addFloatingText(render::Scene& scene, const world::World& world, const ObjectView& objects,
                     uint64_t originHandle, const AvatarView* avatars)
{
    const render::Camera& camera = scene.camera;
    const glm::vec3 forward = glm::normalize(camera.target - camera.position);
    for(const auto& [key, object] : world.objects())
    {
        if(object.hoverText.empty() || object.type != world::ObjectType::Prim)
        {
            continue;
        }
        const auto frame = objects.objectFrame(object, world, originHandle, avatars);
        if(!frame)
        {
            continue;
        }
        render::Label label;
        label.hud = objects.onHud(object, world, avatars);
        label.position = glm::vec3((*frame)[3]) + glm::vec3(0.0f, 0.0f, object.scale.z * 0.6f);
        glm::vec4 color = glm::vec4(object.hoverTextColor[0], object.hoverTextColor[1],
                                    object.hoverTextColor[2], object.hoverTextColor[3]) /
                          255.0f;
        if(!label.hud)
        {
            const glm::vec3 offset = label.position - camera.position;
            const float along = glm::dot(offset, forward);
            if(along <= 0.0f)
            {
                continue; // behind the camera
            }
            // Nearer by the prim's radius, but not past the near plane.
            const float radius = glm::length(object.scale) * 0.5f;
            const float nearest = camera.nearPlane + 0.1f;
            if(along <= nearest + radius)
            {
                label.position = camera.position + offset * (nearest / along);
            }
            else
            {
                label.position -= glm::normalize(offset) * radius;
            }
            const float distance = glm::length(label.position - camera.position);
            if(distance > kFadeDistance + kFadeRange)
            {
                continue;
            }
            if(distance > kFadeDistance)
            {
                color.a *= 1.0f - (distance - kFadeDistance) / kFadeRange;
            }
        }
        if(color.a < 0.01f)
        {
            continue;
        }
        label.lines.push_back({object.hoverText, color, 10.0f}); // SansSerif
        scene.labels.push_back(std::move(label));
    }
}

} // namespace viewer
