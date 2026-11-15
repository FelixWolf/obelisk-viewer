#pragma once

// Flexible prims, bent as the official viewer bends them
// (llflexibleobject.cpp LLVolumeImplFlexible): the prim's length is a chain
// of 2^softness sections (2^3 at most) from its bottom, pulled down by
// gravity and along by the user force, straightened by tension, slowed by
// drag, each bending at most so far from the one below; the chain is then
// remapped (cubically) to as many sections as the prim needs on screen and
// the prim's profile swept along it. Simulated in scene coordinates, so
// moving or turning the prim (or the avatar wearing it) swings it.
//
// Wind (scaled by the prim's wind sensitivity) comes from the region each
// section is over.
//
// As the official viewer (LLVolumeImplFlexible::doIdleUpdate), small ones
// on screen are updated less often: every 1 to 32 "flexi frames" (60 a
// second), spread over frames by the prim's key. A prim whose shape hasn't
// changed since last meshed (one at rest) keeps its meshes.

#include "viewer/render/Renderer.hpp"
#include "viewer/volume/Volume.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace viewer
{

class FlexibleView
{
public:
    using Clock = std::chrono::steady_clock;
    // The wind at a point (scene coordinates), meters a second.
    using WindAt = std::function<glm::vec3(const glm::vec3&)>;

    explicit FlexibleView(render::Renderer& renderer);

    // A flexible prim's faces (face i is texture entry i), bent as simulated
    // up to `now`; its prim placed at `frame` (scene coordinates, rotation
    // and position, no scale), `distance` meters from the camera, at the
    // level of detail `detail` (index into volume::kDetailScales). The
    // meshes are in meters in the prim's frame: draw them with `frame`
    // alone. `windAt` may be empty (no wind, as on HUDs). `screen` is the
    // share of the view it covers (0..1; 1 updates it every flexi frame).
    // Null if it can't be built.
    const std::vector<render::Mesh>* faces(const world::ObjectKey& key, const world::Object& object,
                                           const glm::mat4& frame, float distance, int detail,
                                           float screen, Clock::time_point now,
                                           const WindAt& windAt);
    // How many flexi frames apart a prim covering `screen` of the view is
    // updated (as doIdleUpdate's update_period, RenderFlexTimeFactor 1).
    static uint32_t updatePeriod(float screen);
    // Once a frame: forgets prims not drawn for a while.
    void endFrame();
    // Drops everything (e.g. on logout).
    void clear() { prims_.clear(); }
    size_t count() const { return prims_.size(); }

    // One section of the chain (LLFlexibleObjectSection).
    struct Section
    {
        glm::vec3 position{0.0f}; // its top end
        glm::vec3 direction{0.0f, 0.0f, 1.0f};
        glm::vec3 dPosition{0.0f}; // d position / d length
        glm::vec3 velocity{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat axisRotation{1.0f, 0.0f, 0.0f, 0.0f}; // its twist
        glm::vec2 scale{1.0f};                          // its width and depth, meters
    };
    static constexpr int kMaxResolution = 3; // FLEXIBLE_OBJECT_MAX_SECTIONS
    using Sections = std::array<Section, (1 << kMaxResolution) + 1>;

    // The chain's sections from `sourceResolution` (2^ that many; -1: only
    // section 0, the rest made straight from it) to `resolution`, as
    // remapSections: every so many, or interpolated cubically.
    static void remap(const Sections& source, int sourceResolution, Sections& out, int resolution,
                      float length);

private:
    struct Prim
    {
        world::FlexibleParams params;
        glm::vec3 scale{0.0f};
        volume::VolumeParams volume;
        bool initialized = false;
        int initializedResolution = -1;
        int simulateResolution = 0;
        int renderResolution = 0;
        Sections sections{};
        Clock::time_point lastStep{};
        std::vector<render::Mesh> meshes;
        uint64_t drawnFrame = 0;
        // The flexi frame it was last updated in; what its meshes were made
        // from.
        uint64_t updated = 0;
        int meshedDetail = -1;
        std::vector<volume::PathPoint> meshedPath;
    };
    // The sections' twist and size, and section 0 at the prim's bottom
    // (setAttributesOfAllSections).
    static void setAttributes(Prim& prim, const glm::vec3& position, const glm::quat& rotation);
    // One step of `seconds` (doFlexibleUpdate).
    static void simulate(Prim& prim, const glm::vec3& position, const glm::quat& rotation,
                         float seconds, const WindAt& windAt);

    render::Renderer& renderer_;
    std::unordered_map<world::ObjectKey, Prim, world::ObjectKeyHash> prims_;
    uint64_t frame_ = 0;
};

} // namespace viewer
