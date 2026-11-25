#pragma once

// Avatar physics: the bounce and sway of breasts, belly and butt, as the
// avatar's physics wearable (visual parameters) asks for. Each motion is a
// spring pushed by how a joint moves, and drives morphs nothing else does
// (and the collision volumes they move, which fitted mesh is skinned to).
// Behaves as llphysicsmotion.cpp; our own implementation.

#include "viewer/avatar/Shape.hpp"

#include <map>
#include <optional>
#include <span>
#include <vector>

namespace viewer::avatar
{

// The motions and the parameters they go by, found once per base avatar.
struct PhysicsRig
{
    struct Motion
    {
        size_t joint = 0;          // what moves it
        glm::vec3 direction{0.0f}; // along which, in the joint's axes
        // Parameter ids.
        int mass = 0;
        int gravity = 0;
        int drag = 0;
        int spring = 0;
        int gain = 0;
        int damping = 0;
        int maxEffect = 0;
        std::vector<int> driven;
    };
    std::vector<Motion> motions;

    static PhysicsRig find(const BaseAvatar& base);
};

// One avatar's motions.
class BodyPhysics
{
public:
    // Advances to `now` (seconds). `joints` are as posed (relative to the
    // avatar's root), `root` is the root's frame and `origin` what's added
    // to it for positions that stay the same when the scene's origin moves.
    void update(const PhysicsRig& rig, const BaseAvatar& base, const ParamWeights& weights,
                std::span<const glm::mat4> joints, const glm::mat4& root, const glm::dvec3& origin,
                double now);
    // At rest, as if just seen.
    void reset();

    // The driven parameters' weights, by id; those not here are at rest.
    const std::map<int, float>& weights() const { return weights_; }
    // Moves posed joints' collision volumes by them.
    void moveVolumes(const BaseAvatar& base, std::span<glm::mat4> joints) const;

private:
    struct State
    {
        float position = 0.5f; // 0 to 1, at rest in the middle
        float velocity = 0.0f;
        glm::dvec3 jointPosition{0.0};
        float jointVelocity = 0.0f; // along the motion's direction
        float jointAcceleration = 0.0f;
    };
    std::vector<State> states_; // one per motion
    std::optional<double> updated_;
    std::map<int, float> weights_;
};

} // namespace viewer::avatar
