#include "viewer/avatar/Shape.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace viewer::avatar
{

namespace
{

constexpr int kMaleParam = 80;
constexpr int kHoverParam = 11001;
constexpr float kNormalSoften = 0.65f; // as llpolymorph.cpp

// As the official viewer's U8_to_F32: values within one step of zero are zero.
float fromByte(uint8_t value, float lower, float upper)
{
    const float delta = upper - lower;
    const float v = static_cast<float>(value) / 255.0f * delta + lower;
    return std::fabs(v) < delta / 255.0f ? 0.0f : v;
}

// lldriverparam.cpp getDrivenWeight.
float drivenWeight(const VisualParam::Driven& d, float input, const VisualParam& driver,
                   const VisualParam& driven)
{
    if(input <= d.min1)
    {
        return d.min1 == d.max1 && d.min1 <= driver.min ? driven.max : driven.min;
    }
    if(input <= d.max1)
    {
        const float t = (input - d.min1) / (d.max1 - d.min1);
        return driven.min + t * (driven.max - driven.min);
    }
    if(input <= d.max2)
    {
        return driven.max;
    }
    if(input <= d.min2)
    {
        const float t = (input - d.max2) / (d.min2 - d.max2);
        return driven.max + t * (driven.min - driven.max);
    }
    return d.max2 >= driver.max ? driven.max : driven.min;
}

// A parameter's weight as it applies to this avatar: parameters for the
// other sex stay at their default.
float effectiveWeight(const VisualParam& p, const ParamWeights& weights, Sex sex)
{
    return (static_cast<uint8_t>(p.sex) & static_cast<uint8_t>(sex)) ? weights[p.id]
                                                                     : p.defaultWeight;
}

struct Skeletal
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> scales;
};

Skeletal deformSkeleton(const BaseAvatar& base, const ParamWeights& weights)
{
    Skeletal s;
    for(const Joint& j : base.skeleton.joints)
    {
        s.positions.push_back(j.position);
        s.scales.push_back(j.scale);
    }
    const Sex sex = weights.sex();
    for(const auto& [id, p] : base.params)
    {
        if(p.bones.empty())
        {
            continue;
        }
        const float w = effectiveWeight(p, weights, sex);
        for(const VisualParam::BoneDeformation& d : p.bones)
        {
            s.scales[static_cast<size_t>(d.joint)] += w * d.scale;
            s.positions[static_cast<size_t>(d.joint)] += w * d.offset;
        }
    }
    return s;
}

// Joint transforms relative to the root, as llxform.cpp: a child's offset is
// scaled by its parent's scale, but scale doesn't carry down otherwise.
std::vector<glm::mat4> poseJoints(const BaseAvatar& base, const Skeletal& s)
{
    const auto& joints = base.skeleton.joints;
    std::vector<glm::vec3> worldPosition(joints.size());
    std::vector<glm::quat> worldRotation(joints.size());
    std::vector<glm::mat4> out(joints.size());
    for(size_t i = 0; i < joints.size(); i++)
    {
        const Joint& j = joints[i];
        // The pelvis sits at the root (llavatarappearance.cpp).
        const glm::vec3 position = j.name == "mPelvis" ? glm::vec3(0.0f) : s.positions[i];
        if(j.parent < 0)
        {
            worldPosition[i] = position;
            worldRotation[i] = j.rotation;
        }
        else
        {
            const auto p = static_cast<size_t>(j.parent);
            worldPosition[i] = worldPosition[p] + worldRotation[p] * (s.scales[p] * position);
            worldRotation[i] = worldRotation[p] * j.rotation;
        }
        out[i] = glm::translate(glm::mat4(1.0f), worldPosition[i]) *
                 glm::mat4_cast(worldRotation[i]) * glm::scale(glm::mat4(1.0f), s.scales[i]);
    }
    return out;
}

// The matrices a weighted mesh's vertex weights index, as
// llavatarjointmesh.cpp setupJoint: in skeleton order, each skin joint,
// preceded by its nearest base ancestor unless that was the previous entry.
// Each maps the bind pose (where the mesh was made) to the posed skeleton.
std::vector<glm::mat4> skinMatrices(const BaseAvatar& base, const BodyMesh& mesh,
                                    const std::vector<glm::mat4>& joints)
{
    const auto& skeleton = base.skeleton.joints;
    auto baseAncestor = [&](int joint)
    {
        int a = skeleton[static_cast<size_t>(joint)].parent;
        while(a >= 0 && skeleton[static_cast<size_t>(a)].parent >= 0 &&
              !skeleton[static_cast<size_t>(a)].base)
        {
            a = skeleton[static_cast<size_t>(a)].parent;
        }
        return a; // -1: the avatar root
    };
    auto matrix = [&](int joint)
    {
        if(joint < 0)
        {
            return glm::mat4(1.0f);
        }
        // Where the joint was when the mesh was made: base joints' pivots.
        glm::vec3 bind(0.0f);
        for(int a = joint; a >= 0; a = skeleton[static_cast<size_t>(a)].parent)
        {
            if(skeleton[static_cast<size_t>(a)].base)
            {
                bind += skeleton[static_cast<size_t>(a)].pivot;
            }
        }
        return joints[static_cast<size_t>(joint)] * glm::translate(glm::mat4(1.0f), -bind);
    };

    std::vector<int> skinJoints;
    for(const std::string& name : mesh.jointNames)
    {
        skinJoints.push_back(base.skeleton.find(name));
    }
    std::vector<int> entries;
    for(int j = 0; j < static_cast<int>(skeleton.size()); j++)
    {
        if(std::find(skinJoints.begin(), skinJoints.end(), j) == skinJoints.end())
        {
            continue;
        }
        const int ancestor = baseAncestor(j);
        if(entries.empty() || entries.back() != ancestor)
        {
            entries.push_back(ancestor);
        }
        entries.push_back(j);
    }
    std::vector<glm::mat4> out;
    for(const int e : entries)
    {
        out.push_back(matrix(e));
    }
    return out;
}

} // namespace

ParamWeights::ParamWeights(const BaseAvatar& base) : base_(&base)
{
    for(const auto& [id, p] : base.params)
    {
        weights_[id] = p.defaultWeight;
    }
    applyDrivers();
}

ParamWeights::ParamWeights(const BaseAvatar& base, std::span<const uint8_t> values)
    : ParamWeights(base)
{
    for(size_t i = 0; i < values.size() && i < base.transmitted.size(); i++)
    {
        const VisualParam& p = base.params.at(base.transmitted[i]);
        weights_[p.id] = fromByte(values[i], p.min, p.max);
    }
    applyDrivers();
}

float ParamWeights::operator[](int id) const
{
    const auto it = weights_.find(id);
    return it == weights_.end() ? 0.0f : it->second;
}

void ParamWeights::set(int id, float weight)
{
    const auto it = base_->params.find(id);
    if(it != base_->params.end())
    {
        weights_[id] = std::clamp(weight, it->second.min, it->second.max);
    }
}

Sex ParamWeights::sex() const
{
    return (*this)[kMaleParam] > 0.5f ? Sex::Male : Sex::Female;
}

void ParamWeights::applyDrivers()
{
    // In id order, as the official viewer applies them; a parameter driven
    // by more than one driver takes the last.
    for(const auto& [id, driver] : base_->params)
    {
        for(const VisualParam::Driven& d : driver.driven)
        {
            const auto it = base_->params.find(d.id);
            if(it != base_->params.end())
            {
                weights_[d.id] = drivenWeight(d, weights_[id], driver, it->second);
            }
        }
    }
}

Shape shapeSkeleton(const BaseAvatar& base, const ParamWeights& weights)
{
    const Skeletal s = deformSkeleton(base, weights);
    Shape shape;
    shape.joints = poseJoints(base, s);

    // llavatarappearance.cpp computeBodySize.
    auto pos = [&](const char* name)
    {
        const int i = base.skeleton.find(name);
        return i < 0 ? 0.0f : s.positions[static_cast<size_t>(i)].z;
    };
    auto scale = [&](const char* name)
    {
        const int i = base.skeleton.find(name);
        return i < 0 ? 1.0f : s.scales[static_cast<size_t>(i)].z;
    };
    shape.pelvisToFoot = pos("mHipLeft") * scale("mPelvis") - pos("mKneeLeft") * scale("mHipLeft") -
                         pos("mAnkleLeft") * scale("mKneeLeft") -
                         pos("mFootLeft") * scale("mAnkleLeft");
    shape.bodyHeight = shape.pelvisToFoot + std::sqrt(2.0f) * pos("mSkull") * scale("mHead") +
                       pos("mHead") * scale("mNeck") + pos("mNeck") * scale("mChest") +
                       pos("mChest") * scale("mTorso") + pos("mTorso") * scale("mPelvis");
    shape.hover = weights[kHoverParam];
    return shape;
}

Shape shapeAvatar(const BaseAvatar& base, const ParamWeights& weights)
{
    Shape shape = shapeSkeleton(base, weights);
    const Sex sex = weights.sex();

    // Morphs first, in the mesh's own (bind) space.
    for(const BodyMesh& mesh : base.meshes)
    {
        shape.meshes.push_back({mesh.positions, mesh.normals, mesh.texCoords});
    }
    for(const auto& [id, p] : base.params)
    {
        if(p.morphs.empty())
        {
            continue;
        }
        const float w = effectiveWeight(p, weights, sex);
        if(w == 0.0f)
        {
            continue;
        }
        for(const VisualParam::MorphRef& ref : p.morphs)
        {
            const Morph& morph = base.meshes[ref.part].morphs[ref.morph];
            ShapedMesh& out = shape.meshes[ref.part];
            for(size_t v = 0; v < morph.vertices.size(); v++)
            {
                const uint32_t i = morph.vertices[v];
                out.positions[i] += w * morph.positions[v];
                out.normals[i] += w * kNormalSoften * morph.normals[v];
                out.texCoords[i] += w * morph.texCoords[v];
            }
        }
    }

    // Then skinning to the shaped skeleton.
    for(size_t m = 0; m < base.meshes.size(); m++)
    {
        const BodyMesh& mesh = base.meshes[m];
        ShapedMesh& out = shape.meshes[m];
        if(mesh.weights.empty())
        {
            // Rigid on a joint (the eyes); the meshes are in its space.
            const int joint = kBodyParts[m].joint ? base.skeleton.find(kBodyParts[m].joint) : -1;
            const glm::mat4 matrix =
                joint < 0 ? glm::mat4(1.0f) : shape.joints[static_cast<size_t>(joint)];
            for(size_t v = 0; v < out.positions.size(); v++)
            {
                out.positions[v] = glm::vec3(matrix * glm::vec4(out.positions[v], 1.0f));
                out.normals[v] = glm::normalize(glm::mat3(matrix) * out.normals[v]);
            }
            continue;
        }
        const std::vector<glm::mat4> skin = skinMatrices(base, mesh, shape.joints);
        if(skin.empty())
        {
            continue;
        }
        const int last = static_cast<int>(skin.size()) - 1;
        for(size_t v = 0; v < out.positions.size(); v++)
        {
            // The weight is an index into the skin matrices plus a blend
            // towards the next one (as avatarSkinV.glsl).
            const float weight = mesh.weights[v];
            const int i = std::clamp(static_cast<int>(std::floor(weight)), 0, last);
            const int next = std::min(i + 1, last);
            const float t = weight - std::floor(weight);
            const glm::mat4 matrix =
                skin[static_cast<size_t>(i)] * (1.0f - t) + skin[static_cast<size_t>(next)] * t;
            out.positions[v] = glm::vec3(matrix * glm::vec4(out.positions[v], 1.0f));
            const glm::vec3 n = glm::mat3(matrix) * out.normals[v];
            const float length = glm::length(n);
            out.normals[v] = length > 1e-6f ? n / length : glm::vec3(0, 0, 1);
        }
    }
    return shape;
}

} // namespace viewer::avatar
