#pragma once

// What to draw this frame. Built by the caller every frame (cheap: it only
// refers to meshes, textures and materials owned elsewhere).
//
// Coordinates follow Second Life: right-handed, X east, Y north, Z up, meters.

#include "viewer/render/Frustum.hpp"
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

// Second Life's atmosphere model ("windlight"): the sky is drawn from it,
// and surfaces are hazed by it with distance. Values are in the sky
// settings' own (roughly sRGB) units; see environment::SkySettings.
struct Atmosphere
{
    bool enabled = false;                       // off: a plain skyColor background, no haze
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f}; // towards the sun (or moon)
    glm::vec3 sunlight{0.73f, 0.78f, 0.9f};     // the settings' light color
    glm::vec3 ambient{0.25f};
    glm::vec3 blueHorizon{0.5f, 0.5f, 0.64f};
    glm::vec3 blueDensity{0.24f, 0.45f, 0.76f};
    float hazeHorizon = 0.19f;
    float hazeDensity = 0.7f;
    float densityMultiplier = 0.0001f;
    float distanceMultiplier = 0.8f;
    float maxY = 1605.0f;
    glm::vec3 glow{5.0f, 0.001f, -0.48f};
    float cloudShadow = 0.27f;
    float sunMoonGlow = 1.0f; // 1 while the sun is up
    bool sunUp = true;
    // The discs and stars.
    glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};
    glm::vec3 moonDirection{0.0f, 0.0f, -1.0f};
    float sunRadius = 0.012f; // radians
    float moonRadius = 0.02f;
    float moonBrightness = 0.5f;
    float starBrightness = 0.0f; // 0..500ish, as the settings
    // Clouds: a noise texture (none: no clouds) and how it's used.
    const GpuTexture* clouds = nullptr;
    glm::vec3 cloudColor{0.41f};
    float cloudScale = 0.42f;
    glm::vec3 cloudPosDensity1{1.0f, 0.526f, 1.0f}; // xy: offset (scrolling), z: density
    float cloudVariance = 0.0f;
    glm::vec3 cloudPosDensity2{1.0f, 0.526f, 1.0f};
    float domeRadius = 15000.0f;
    float domeOffset = 0.96f;
};

struct Lighting
{
    // The light surfaces are lit by: the sun, or the moon at night.
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.3f, 0.8f)); // towards it
    glm::vec3 sunColor{3.0f, 2.85f, 2.6f};                                // linear
    glm::vec3 skyAmbient{0.35f, 0.45f, 0.6f};                             // linear
    glm::vec3 groundAmbient{0.15f, 0.13f, 0.1f};
    glm::vec3 skyColor{0.45f, 0.6f, 0.85f}; // background when there's no atmosphere
    float exposure = 1.0f;
    Atmosphere atmosphere;
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
    // What the camera sees: whoever fills the lists leaves out what's
    // entirely outside it.
    Frustum frustum;
    Lighting lighting;
    std::vector<TerrainDraw> terrain;
    std::vector<ObjectDraw> objects;
};

} // namespace viewer::render
