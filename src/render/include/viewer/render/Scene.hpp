#pragma once

// What to draw this frame. Built by the caller every frame (cheap: it only
// refers to meshes, textures and materials owned elsewhere).
//
// Coordinates follow Second Life: right-handed, X east, Y north, Z up, meters.

#include "viewer/render/Material.hpp"
#include "viewer/render/Mesh.hpp"

#include <glm/glm.hpp>

#include <span>
#include <variant>
#include <vector>

namespace viewer::render
{

struct Camera
{
    glm::vec3 position{0.0f, -10.0f, 5.0f};
    glm::vec3 target{0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    float verticalFov = 1.0472f; // radians
    float nearPlane = 0.25f;
    float farPlane = 1024.0f;
};

struct Lighting
{
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.3f, 0.8f)); // towards the sun
    glm::vec3 sunColor{3.0f, 2.85f, 2.6f};
    glm::vec3 skyAmbient{0.35f, 0.45f, 0.6f};
    glm::vec3 groundAmbient{0.15f, 0.13f, 0.1f};
    glm::vec3 skyColor{0.45f, 0.6f, 0.85f}; // background
    float exposure = 1.0f;
};

struct ObjectDraw
{
    const Mesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    std::variant<BlinnPhongMaterial, PbrMaterial> material;
    // Texture coordinate transform, rows of a 2x3 matrix applied to (u, v, 1).
    glm::vec3 uvRow0{1.0f, 0.0f, 0.0f};
    glm::vec3 uvRow1{0.0f, 1.0f, 0.0f};
    // Skinned meshes: joint matrices (relative to `transform`) the vertices'
    // joint indices refer to. Must stay valid until the frame is drawn.
    std::span<const glm::mat4> joints{};
};

struct TerrainDraw
{
    const Mesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    std::variant<TerrainBlinnPhongMaterial, TerrainPbrMaterial> material;
};

struct Scene
{
    // Global position (meters) of the scene's origin. Terrain textures are
    // laid out in global coordinates, so they line up across regions and
    // don't shift when the origin moves.
    glm::dvec2 worldOrigin{0.0};
    Camera camera;
    Lighting lighting;
    std::vector<TerrainDraw> terrain;
    std::vector<ObjectDraw> objects;
};

} // namespace viewer::render
