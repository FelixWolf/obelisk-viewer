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

void deformSkeleton(const BaseAvatar& base, const ParamWeights& weights, Shape& shape)
{
    for(const Joint& j : base.skeleton.joints)
    {
        // The pelvis sits at the root (llavatarappearance.cpp).
        shape.jointPositions.push_back(j.name == "mPelvis" ? glm::vec3(0.0f) : j.position);
        shape.jointScales.push_back(j.scale);
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
            shape.jointScales[static_cast<size_t>(d.joint)] += w * d.scale;
            shape.jointPositions[static_cast<size_t>(d.joint)] += w * d.offset;
        }
    }
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
    Shape shape;
    deformSkeleton(base, weights, shape);

    // llavatarappearance.cpp computeBodySize (from the unshifted positions).
    auto pos = [&](const char* name)
    {
        const int i = base.skeleton.find(name);
        if(i < 0)
        {
            return 0.0f;
        }
        return name == std::string_view("mPelvis") ? 0.0f
                                                   : shape.jointPositions[static_cast<size_t>(i)].z;
    };
    auto scale = [&](const char* name)
    {
        const int i = base.skeleton.find(name);
        return i < 0 ? 1.0f : shape.jointScales[static_cast<size_t>(i)].z;
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

    return shape;
}

std::vector<glm::mat4> poseJoints(const BaseAvatar& base, const Shape& shape,
                                  std::span<const JointPose> pose)
{
    // As llxform.cpp: a child's offset is scaled by its parent's scale, but
    // scale doesn't carry down otherwise.
    const auto& joints = base.skeleton.joints;
    std::vector<glm::vec3> worldPosition(joints.size());
    std::vector<glm::quat> worldRotation(joints.size());
    std::vector<glm::mat4> out(joints.size());
    for(size_t i = 0; i < joints.size(); i++)
    {
        const Joint& j = joints[i];
        glm::vec3 position = shape.jointPositions[i];
        glm::quat rotation = j.rotation;
        if(i < pose.size())
        {
            position = pose[i].position.value_or(position);
            rotation = pose[i].rotation.value_or(rotation);
        }
        if(j.parent < 0)
        {
            worldPosition[i] = position;
            worldRotation[i] = rotation;
        }
        else
        {
            const auto p = static_cast<size_t>(j.parent);
            worldPosition[i] =
                worldPosition[p] + worldRotation[p] * (shape.jointScales[p] * position);
            worldRotation[i] = worldRotation[p] * rotation;
        }
        out[i] = glm::translate(glm::mat4(1.0f), worldPosition[i]) *
                 glm::mat4_cast(worldRotation[i]) *
                 glm::scale(glm::mat4(1.0f), shape.jointScales[i]);
    }
    return out;
}

void skinPalette(const BaseAvatar& base, size_t part, std::span<const glm::mat4> joints,
                 std::vector<glm::mat4>& out)
{
    out.clear();
    const bool weighted = !base.meshes[part].weights.empty();
    for(const int joint : base.skinJoints[part])
    {
        if(joint < 0)
        {
            out.emplace_back(1.0f);
        }
        else if(!weighted)
        {
            out.push_back(joints[static_cast<size_t>(joint)]); // in the joint's space
        }
        else
        {
            out.push_back(
                joints[static_cast<size_t>(joint)] *
                glm::translate(glm::mat4(1.0f), -base.bindPositions[static_cast<size_t>(joint)]));
        }
    }
}

Influence vertexInfluence(const BaseAvatar& base, size_t part, size_t vertex)
{
    const BodyMesh& mesh = base.meshes[part];
    const int last = static_cast<int>(base.skinJoints[part].size()) - 1;
    if(mesh.weights.empty() || last <= 0)
    {
        return {};
    }
    // An index plus a blend towards the next one (as avatarSkinV.glsl).
    const float weight = mesh.weights[vertex];
    const int first = std::clamp(static_cast<int>(std::floor(weight)), 0, last);
    return {static_cast<uint8_t>(first), static_cast<uint8_t>(std::min(first + 1, last)),
            weight - std::floor(weight)};
}

ShapedMesh skinMesh(const BaseAvatar& base, size_t part, const ShapedMesh& mesh,
                    std::span<const glm::mat4> joints)
{
    std::vector<glm::mat4> palette;
    skinPalette(base, part, joints, palette);
    ShapedMesh out = mesh;
    if(palette.empty())
    {
        return out;
    }
    for(size_t v = 0; v < out.positions.size(); v++)
    {
        const Influence in = vertexInfluence(base, part, v);
        const glm::mat4 matrix =
            palette[in.first] * (1.0f - in.blend) + palette[in.second] * in.blend;
        out.positions[v] = glm::vec3(matrix * glm::vec4(mesh.positions[v], 1.0f));
        const glm::vec3 n = glm::mat3(matrix) * mesh.normals[v];
        const float length = glm::length(n);
        out.normals[v] = length > 1e-6f ? n / length : glm::vec3(0, 0, 1);
    }
    return out;
}

} // namespace viewer::avatar
