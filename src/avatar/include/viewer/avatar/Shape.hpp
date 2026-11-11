#pragma once

// An avatar's shape: visual parameter weights (from the appearance
// message), the skeleton they deform and the body meshes morphed and skinned
// to it. Follows llvoavatar.cpp (appearance message), lldriverparam.cpp,
// llpolymorph.cpp, llpolyskeletaldistortion.cpp, llavatarjointmesh.cpp and
// llavatarappearance.cpp computeBodySize; our own implementation.

#include "viewer/avatar/BaseAvatar.hpp"

#include <cstdint>
#include <map>
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

// A body mesh after morphs and skinning, in the avatar root's space.
struct ShapedMesh
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;
};

struct Shape
{
    std::vector<glm::mat4> joints;  // per joint, relative to the avatar root
    std::vector<ShapedMesh> meshes; // one per kBodyParts entry
    float pelvisToFoot = 0.0f;
    float bodyHeight = 0.0f;
    float hover = 0.0f;
};

// The skeleton only (cheap): joints, body size and hover.
Shape shapeSkeleton(const BaseAvatar& base, const ParamWeights& weights);
// The skeleton and the meshes.
Shape shapeAvatar(const BaseAvatar& base, const ParamWeights& weights);

} // namespace viewer::avatar
