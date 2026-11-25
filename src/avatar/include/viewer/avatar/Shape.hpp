#pragma once

// An avatar's shape: visual parameter weights (from the appearance
// message), the skeleton they deform and the body meshes they morph; and
// posing that skeleton, for skinning the meshes to it. Follows llvoavatar.cpp
// (appearance message), lldriverparam.cpp, llpolymorph.cpp,
// llpolyskeletaldistortion.cpp, llavatarjointmesh.cpp, avatarSkinV.glsl and
// llavatarappearance.cpp computeBodySize; our own implementation.

#include "viewer/avatar/BaseAvatar.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace viewer::avatar
{

// Visual parameter weights by id.
class ParamWeights
{
public:
    // Defaults for everything.
    explicit ParamWeights(const BaseAvatar& base);
    // From an AvatarAppearance message's VisualParam values (one byte each,
    // in BaseAvatar::transmitted order); missing values keep their default.
    ParamWeights(const BaseAvatar& base, std::span<const uint8_t> values);

    float operator[](int id) const;
    void set(int id, float weight);
    Sex sex() const;

    // Sets driven parameters from their drivers (done by the constructors;
    // call again after set()).
    void applyDrivers();

private:
    const BaseAvatar* base_;
    std::map<int, float> weights_;
};

// A body mesh after morphs, still where it was made (skeleton space, or the
// joint's space for unweighted meshes); skinning places it.
struct ShapedMesh
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;
};

struct Shape
{
    // Per joint, relative to its parent, after skeletal parameters. The
    // pelvis is at the root.
    std::vector<glm::vec3> jointPositions;
    std::vector<glm::vec3> jointScales;
    std::vector<ShapedMesh> meshes; // one per kBodyParts entry (shapeAvatar only)
    float pelvisToFoot = 0.0f;
    float bodyHeight = 0.0f;
    float hover = 0.0f;
};

// A bake's morph mask (its fifth component; rows bottom first, as the
// bakes are uploaded): where clothing is, for the morphs that follow it
// (loose shirts and pants, flares, cuffs).
struct MorphMask
{
    int bake = -1; // texture entry index: 8 head, 9 upper, 10 lower
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> pixels; // width * height
};

// The skeleton only (cheap): joints, body size and hover.
Shape shapeSkeleton(const BaseAvatar& base, const ParamWeights& weights);
// The skeleton and the morphed meshes. Morphs that follow clothing are
// weighed per vertex by their bake's mask; without one, applied whole (as
// the official viewer until the mask arrives).
Shape shapeAvatar(const BaseAvatar& base, const ParamWeights& weights,
                  std::span<const MorphMask> masks = {});

// One of the morphed meshes.
ShapedMesh shapePart(const BaseAvatar& base, const ParamWeights& weights, size_t part,
                     std::span<const MorphMask> masks = {});

// A joint's animated local transform; what isn't set keeps the rest pose
// (rotation) or the shape (position).
struct JointPose
{
    std::optional<glm::quat> rotation;
    std::optional<glm::vec3> position;
    std::optional<glm::vec3> scale;
};

// How tall the avatar is, and its pelvis above its feet, by where its joints
// are (posed ones' positions and scales, the rest the shape's).
struct BodySize
{
    float pelvisToFoot = 0.0f;
    float height = 0.0f;
};
BodySize bodySize(const BaseAvatar& base, const Shape& shape, std::span<const JointPose> pose = {});

// Joint transforms relative to the avatar root. `pose` is empty (the rest
// pose) or has one entry per joint.
std::vector<glm::mat4> poseJoints(const BaseAvatar& base, const Shape& shape,
                                  std::span<const JointPose> pose = {});

// The same as positions, rotations and scales (relative to the avatar
// root), for IK.
struct JointFrame
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};
std::vector<JointFrame> poseFrames(const BaseAvatar& base, const Shape& shape,
                                   std::span<const JointPose> pose = {});

// The matrices a body part's vertex influences refer to, for joints posed by
// poseJoints: each takes a vertex from where the mesh was made to where its
// joint is now.
void skinPalette(const BaseAvatar& base, size_t part, std::span<const glm::mat4> joints,
                 std::vector<glm::mat4>& out);

// A vertex's influences: palette entries `first` and `second`, blended by
// `blend` (0: all first). From the .llm weight: an index plus a fraction.
struct Influence
{
    uint8_t first = 0;
    uint8_t second = 0;
    float blend = 0.0f;
};
Influence vertexInfluence(const BaseAvatar& base, size_t part, size_t vertex);

// Skins a shaped mesh on the CPU (for tests and tools; drawing does it on
// the GPU).
ShapedMesh skinMesh(const BaseAvatar& base, size_t part, const ShapedMesh& mesh,
                    std::span<const glm::mat4> joints);

} // namespace viewer::avatar
