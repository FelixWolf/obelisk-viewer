#pragma once

// Particles, as the official viewer makes them (llviewerpartsource.cpp
// LLViewerPartSourceScript, llviewerpartsim.cpp): each prim with a particle
// system is a source that emits bursts of particles every so often (in a
// pattern: dropped, exploding, in an angle or cone, turning with its
// angular velocity); particles live their max age, moving under their
// velocity and acceleration (and the wind, toward a target, following
// their source or bouncing off its height as asked), their color, size
// and glow going from start to end.
//
// As the official viewer, the number alive is limited (RenderMaxPartCount,
// 4096): sources emit less as it nears that, small or far particles (that
// would barely show before they die) aren't emitted at all.
//
// Particles without a texture use the official viewer's default image
// (pixiesmall, from the asset server).
//
// Positions are in scene coordinates (meters from the scene's origin
// region); they move with it when it changes.

#include "viewer/core/Uuid.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/ParticleSystem.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace viewer
{

class ParticleView
{
public:
    static constexpr int kMaxParticles = 4096; // RenderMaxPartCount
    static constexpr int kHardLimit = 8192;    // LLViewerPartSim::MAX_PART_COUNT

    // Where an object is now (scene coordinates), by id: particle targets.
    using Locate = std::function<std::optional<glm::vec3>(const Uuid&)>;
    // The wind at a point (scene coordinates), meters a second.
    using WindAt = std::function<glm::vec3(const glm::vec3&)>;

    // Each frame, before update(): every source, where its prim is now.
    // `revision` changes with each update of the prim (world::Object's
    // content revision): one that had outlived its max age starts over on
    // the next, even if its system is the same (as the official viewer
    // makes a new source then), as scripts that re-send theirs expect.
    void source(const world::ObjectKey& key, const world::ParticleSystem& system, uint32_t revision,
                const glm::vec3& position, const glm::quat& rotation);
    // Emits and moves particles by `seconds`: sources not given this frame
    // stop emitting (their particles live on). `pixelsPerMeter`: how many
    // pixels a meter one meter away covers on screen.
    void update(float seconds, uint64_t originHandle, const glm::vec3& camera, float pixelsPerMeter,
                const Locate& locate, const WindAt& windAt);
    // A texture to draw with, at `texels` across (null while it loads).
    using TextureFor = std::function<const render::GpuTexture*(const Uuid&, float texels)>;
    // The particles to draw.
    void addTo(render::Scene& scene, const TextureFor& textures, float pixelsPerMeter) const;
    void clear();
    size_t count() const { return static_cast<size_t>(alive_); }
    size_t sources() const { return sources_.size(); }

private:
    struct Source
    {
        world::ParticleSystem system;
        uint32_t revision = 0;
        glm::vec3 position{0.0f};
        glm::quat objectRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // its angular velocity's turn so far
        glm::vec3 target{0.0f};
        float lastUpdate = 0.0f;
        float lastPart = 0.0f;
        bool seen = false;     // given this frame
        bool expired = false;  // outlived its max age (until its system changes)
        int lastParticle = -1; // ribbons: the one emitted last
    };
    struct Particle
    {
        bool alive = false;
        std::shared_ptr<Source> source;
        uint32_t flags = 0;
        float age = 0.0f;
        float maxAge = 0.0f;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 acceleration{0.0f};
        glm::vec3 offset{0.0f}; // from its source (FOLLOW_SRC)
        glm::vec4 color{1.0f}, startColor{1.0f}, endColor{1.0f};
        glm::vec2 scale{1.0f}, startScale{1.0f}, endScale{1.0f};
        float glow = 0.0f, startGlow = 0.0f, endGlow = 0.0f;
        uint8_t blendSource = world::ParticleSystem::SourceAlpha;
        uint8_t blendDest = world::ParticleSystem::OneMinusSourceAlpha;
        Uuid texture{};
        glm::vec3 axis{0.0f}; // ribbons: the width direction
        int parent = -1;      // ribbons: the one emitted after it
        int child = -1;       // and before
    };

    void emit(const std::shared_ptr<Source>& source, float seconds, const glm::vec3& camera,
              float pixelsPerMeter);
    int allocate();
    void kill(int index);
    float random() { return uniform_(rng_); } // 0..1

    std::unordered_map<world::ObjectKey, std::shared_ptr<Source>, world::ObjectKeyHash> sources_;
    std::vector<Particle> particles_;
    std::vector<int> free_;
    int alive_ = 0;
    uint64_t originHandle_ = 0;
    bool haveOrigin_ = false;
    uint64_t frame_ = 0;
    float time_ = 0.0f;
    // LLViewerPartSim's throttles: how much of a burst gets emitted, and
    // how readily far sources emit.
    float burstRate_ = 0.5f;
    float adaptiveRate_ = 0.0625f;
    std::mt19937 rng_{12345};
    std::uniform_real_distribution<float> uniform_{0.0f, 1.0f};
};

} // namespace viewer
