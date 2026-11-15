#pragma once

// What to draw this frame. Built by the caller every frame (cheap: it only
// refers to meshes, textures and materials owned elsewhere).
//
// Coordinates follow Second Life: right-handed, X east, Y north, Z up, meters.

#include "viewer/render/Frustum.hpp"
#include "viewer/render/Material.hpp"
#include "viewer/render/Mesh.hpp"
#include "viewer/render/OcclusionMap.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
    // Exposure: this setting times the dynamic exposure, which goes from
    // exposureMax for a dark view to exposureMin for a bright one.
    float exposure = 1.0f;
    float exposureMin = 1.0f;
    float exposureMax = 1.0f;
    // How much of the tonemapped color, the rest linear (the official viewer's
    // RenderTonemapMix).
    float tonemapMix = 0.7f;
    // The sky's reflection probe ambiance: how much of the ambient light
    // comes from the reflection probes. 0 for skies made before PBR, which
    // keep `skyAmbient` and `groundAmbient` ("classic mode").
    float probeAmbiance = 0.0f;
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
    // Planar mapping: (u, v) from the vertex's position in meters on the
    // face's plane (llface.cpp planarProjection), not the mesh's own, before
    // the transform above. Unskinned draws only.
    bool planar = false;
    // Skinned meshes: joint matrices (relative to `transform`) the vertices'
    // joint indices refer to. Must stay valid until the frame is drawn.
    std::span<const glm::mat4> joints{};
    // Transparent rigged mesh is drawn after other transparent faces, worn
    // mesh grouped by the skeleton's owner (whatever identifies it; nearer
    // owners later) and, within one, by `layer` (higher first), then in
    // scene order: the official viewer's attachment draw order, since
    // distance can't tell layers of clothing apart.
    const void* wearer = nullptr;
    uint64_t layer = 0;
    // Out of view: drawn only into shadow maps and reflection probes.
    bool shadowOnly = false;
    // A HUD attachment: in HUD space (render::Hud), drawn only into the HUD.
    bool hud = false;
    // Picking (Scene::pick): what the pick reports for this draw; 0: rays
    // go through it. Pick-only draws are drawn nowhere else: faces that are
    // invisible but can still be clicked.
    uint64_t pickId = 0;
    bool pickOnly = false;
    // Glow (the texture entry's): how much the face glows, a blurred halo
    // added over the view (RendererGlow.cpp).
    float glow = 0.0f;
};

// What's along a ray: the nearest pickable draw (ObjectDraw::pickId) it
// meets, and where. A HUD point is tried first (HUDs are in front of
// everything). The answer comes a frame or two later (Renderer::takePick),
// tagged with the request's id; a request made while one is still on its way
// is skipped.
struct PickRequest
{
    uint64_t id = 0;
    glm::vec3 origin{0.0f};
    glm::vec3 direction{1.0f, 0.0f, 0.0f}; // unit length
    float maxDistance = 256.0f;
    std::optional<glm::vec2> hudPoint; // HUD space Y and Z (render::Hud)
};

struct PickResult
{
    uint64_t request = 0;
    uint64_t pickId = 0; // 0: nothing was hit
    bool hud = false;    // a HUD draw (positions in HUD space)
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 binormal{0.0f};
    glm::vec2 uv{0.0f}; // texture coordinates, after the face's transform
    glm::vec2 st{0.0f}; // the face's own coordinates
};

struct TerrainDraw
{
    const Mesh* mesh = nullptr;
    glm::mat4 transform{1.0f};
    std::variant<TerrainBlinnPhongMaterial, TerrainPbrMaterial> material;
    bool shadowOnly = false; // out of view, but casts shadows
};

// Shadows from the sun (or moon): cascaded shadow maps, each covering a
// wider slice of the view (the renderer works them out from the camera).
struct Shadows
{
    bool enabled = false;
    glm::vec3 towardLight{0.0f, 0.0f, 1.0f};
    float distance = 128.0f; // shadows end this far from the camera
};

// Reflections (and, on skies made for PBR, ambient light) from reflection
// probes, as the official viewer's: a default probe (the sky and terrain,
// captured above the camera) and local probes (Scene::probes), cubes of
// everything around them, which surfaces inside take their light from.
struct Reflections
{
    bool enabled = true;
    float updateSeconds = 2.0f;   // the default probe's (RenderDefaultProbeUpdatePeriod)
    float refreshSeconds = 10.0f; // a local probe's, at the soonest
    float nearClip = 1.0f;        // meters; the default probe's (getNearClip)
    float aboveCamera = 64.0f;    // the default probe's height over the camera
    float drawDistance = 64.0f;   // captures reach this far (RenderReflectionProbeDrawDistance)
    // The probe face captured this frame (Renderer::nextProbeFace), if any:
    // out of view, what's in it is still listed (as shadowOnly draws).
    std::optional<Frustum> probeFace;
};

// HUD attachments: drawn into an image of their own (as the official
// viewer draws them, unlit and alpha blended, through an orthographic
// camera), which is then shown where `placement` says: over the screen, or
// on a panel in the world (a wrist, say, or in front of a VR headset).
//
// HUD space is the official one: the camera looks along +X (nearer is
// smaller X), +Y is left and +Z up; the image is 1 unit tall and `aspect`
// wide at zoom 1, centered on the origin.
struct Hud
{
    float aspect = 1.0f; // width over height of the HUD image
    float zoom = 1.0f;
    float pixelHeight = 1080.0f; // the image's height in pixels (for texture sizes)
    enum class Placement : uint8_t
    {
        Screen, // over the view, as the official viewer
        World,  // on a panel in the scene
    };
    Placement placement = Placement::Screen;
    // Placement::World: the panel's frame (its center; +Y left, +Z up, faced
    // from -X, as HUD space) and its height in meters.
    glm::mat4 panel{1.0f};
    float panelHeight = 0.3f;
};

// A local reflection probe: inside its sphere (or box), surfaces reflect
// the cube captured at its origin (with parallax: rays are followed to the
// volume's edge), and on skies made for PBR take ambient light from it.
// Manual probes are prims set to be probes; automatic ones are placed where
// objects are dense (as the official viewer's octree).
struct ReflectionProbe
{
    uint64_t id = 0; // stays the same while the probe does
    glm::vec3 origin{0.0f};
    float radius = 16.0f; // the sphere's (a box's: its half diagonal)
    // Box probes: scene -> the box as a cube from -1 to 1.
    std::optional<glm::mat4> box;
    float ambiance = 0.0f; // its own (at least the sky's is used)
    float nearClip = 0.1f; // nothing nearer shows in its cube
    bool manual = false;
};

// A local light (a prim set to give light), as the official viewer draws
// it: it reaches `range` (1.5 times the prim's light radius), fading by
// (1 - clamp((d + falloff) / (1 + falloff)))^2 * 2 with d the distance over
// the range. Spotlights (projectors) light a cone along `direction`.
struct LocalLight
{
    glm::vec3 position{0.0f};
    float range = 0.0f;
    glm::vec3 color{0.0f}; // linear, times the light's intensity
    float falloff = 0.0f;
    // A projector shines its texture through a frustum instead of all
    // around (see projectorFor).
    struct Projector
    {
        glm::mat4 matrix{1.0f}; // scene -> texture: xy in 0..1 after the divide, z >= 0 in front
        glm::vec3 origin{0.0f}; // the frustum's apex
        glm::vec3 axis{0.0f, 0.0f, -1.0f}; // the way it shines
        float focus = 0.0f;    // sharp this far along the axis (from the light), blurrier beyond
        float depth = 1.0f;    // near to far plane: blurring reaches its most over this
        float ambiance = 0.0f; // diffuse fill, from the blurred texture
        const GpuTexture* texture = nullptr; // nothing yet: gray (lights.glsl)
    };
    std::optional<Projector> projector;
};

// The projector of a light at `position`, turned by `rotation`, of a prim
// sized `scale`, reaching `range`: as the official viewer's (pipeline.cpp
// setupSpotLight), its near plane is the prim's -Z face, its field of view
// `fov` spans the prim's Y size there, and its aspect is the prim's X/Y.
LocalLight::Projector projectorFor(const glm::vec3& position, const glm::quat& rotation,
                                   const glm::vec3& scale, float range, float fov, float focus,
                                   float ambiance);

enum class Visibility : uint8_t
{
    Hidden,     // not drawn
    ShadowOnly, // out of view, but its shadow can fall into it, or reflection probes see it
    Visible,
};

// One line of a label (it may wrap): its text, color (sRGB, and alpha) and
// size (points, at 96 dpi).
struct LabelLine
{
    std::string text;
    glm::vec4 color{1.0f};
    float points = 10.0f;
    bool header = false; // NameTag: in the band across the bubble's top
};

// Text over a point, the same size on screen at any distance, centered
// above it, with a drop shadow. Hidden where the scene is in front of the
// point; HUD labels are drawn into the HUD image, never hidden. As the
// official viewer draws them (llhudtext.cpp, llhudnametag.cpp):
//   FloatingText (llSetText): the last baseline 12 pixels above the point.
//   NameTag: in a rounded bubble whose bottom is `raise` pixels above the
//   point, lines 3 pixels apart; header lines (a chat bubble's name) in a
//   band across its top, the rest centered or (chat) left aligned.
struct Label
{
    enum class Style : uint8_t
    {
        FloatingText,
        NameTag,
    };
    Style style = Style::FloatingText;
    glm::vec3 position{0.0f}; // scene coordinates (HUD space for HUD labels)
    std::vector<LabelLine> lines;
    glm::vec4 background{0.0f};       // NameTag: the bubble's color (sRGB, and alpha)
    glm::vec4 headerBackground{0.0f}; // and its header band's
    bool alignLeft = false;           // NameTag: lines other than headers
    float raise = 0.0f;
    bool hud = false;
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
    Shadows shadows;
    Reflections reflections;
    Hud hud;
    std::optional<PickRequest> pick;
    // The prim to outline (any of its draws' pickId; 0: none), such as the
    // one the pointer is on when clicking it does something.
    uint64_t highlight = 0;
    std::vector<ReflectionProbe> probes;
    std::vector<TerrainDraw> terrain;
    std::vector<ObjectDraw> objects;
    // Every light that may reach the view; the renderer uses the nearest
    // (Renderer::kMaxLights).
    std::vector<LocalLight> lights;
    std::vector<Label> labels;
    // Passes that can be left out (for measuring what they cost): glow, and
    // measuring the view for dynamic exposure (the last exposure stays).
    bool glow = true;
    bool dynamicExposure = true;

    // What a recent frame hid (Renderer::occlusion); null: nothing known.
    // Only used if drawn at this scene's origin.
    std::shared_ptr<const OcclusionMap> occlusion;
    // Hidden things this big (radius, meters) still cast shadows: smaller
    // ones behind a wall rarely cast one into view.
    static constexpr float kHiddenShadowRadius = 8.0f;

    // Whether a recent frame hid this sphere.
    bool occluded(const glm::vec3& center, float radius) const
    {
        return occlusion && occlusion->origin() == worldOrigin && occlusion->hidden(center, radius);
    }

    // Whatever is within this sphere: in view, only its shadow in view (or
    // near enough for reflection probes), or neither (frustum, shadows and
    // reflections must be set). In view but hidden behind something counts
    // as out of view, its shadow only if it's big.
    Visibility visibility(const glm::vec3& center, float radius) const
    {
        const bool inView = frustum.containsSphere(center, radius);
        const bool hidden = inView && occluded(center, radius);
        if(inView && !hidden)
        {
            return Visibility::Visible;
        }
        if(shadows.enabled && (!hidden || radius >= kHiddenShadowRadius) &&
           glm::length(center - camera.position) - radius < shadows.distance + 64.0f &&
           frustum.containsShadowOf(center, radius, shadows.towardLight))
        {
            return Visibility::ShadowOnly;
        }
        if(reflections.enabled && reflections.probeFace &&
           reflections.probeFace->containsSphere(center, radius))
        {
            return Visibility::ShadowOnly;
        }
        return Visibility::Hidden;
    }
};

} // namespace viewer::render
