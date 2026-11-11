#pragma once

// The default avatar as the official viewer defines it: skeleton
// (avatar_skeleton.xml), body meshes (.llm) and attachment points
// (avatar_lad.xml). Formats follow llavatarappearance.cpp, llpolymesh.cpp and
// llvoavatar.cpp; our own implementation.
//
// Phase 1: rest pose only (no animation, no visual parameters/morphs yet).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace viewer::avatar
{

struct Joint
{
    std::string name;
    int parent = -1;
    glm::vec3 position{0.0f}; // relative to the parent
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct Skeleton
{
    std::vector<Joint> joints; // parents come before children
    std::map<std::string, int, std::less<>> byName;

    int find(std::string_view name) const;
    // Joint transforms relative to the avatar root, in the rest pose with the
    // pelvis at the root (as the official viewer places it).
    std::vector<glm::mat4> restPose() const;
};

// A .llm body mesh in skeleton space (or, for unweighted meshes such as the
// eyes, in its joint's space).
struct BodyMesh
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;
    std::vector<float> weights; // joint index + blend (empty if unweighted)
    std::vector<uint32_t> indices;
    std::vector<std::string> jointNames;
};

struct AttachmentPoint
{
    int id = 0;
    std::string name;
    int joint = -1; // -1: the avatar root
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// The meshes the body is drawn with, and the baked texture each uses
// (texture entry index of the bake).
struct BodyPart
{
    const char* file;
    int bake; // texture entry index: 8 head, 9 upper, 10 lower, 11 eyes, 19 skirt, 20 hair
    const char* bakeName; // in the appearance service URL
    const char* joint;    // for unweighted meshes: the joint they ride on
};

inline constexpr BodyPart kBodyParts[] = {
    {"avatar_head.llm", 8, "head", nullptr},        {"avatar_eyelashes.llm", 8, "head", nullptr},
    {"avatar_upper_body.llm", 9, "upper", nullptr}, {"avatar_lower_body.llm", 10, "lower", nullptr},
    {"avatar_hair.llm", 20, "hair", nullptr},       {"avatar_skirt.llm", 19, "skirt", nullptr},
    {"avatar_eye.llm", 11, "eyes", "mEyeLeft"},     {"avatar_eye.llm", 11, "eyes", "mEyeRight"},
};

struct BaseAvatar
{
    Skeleton skeleton;
    std::vector<BodyMesh> meshes; // one per kBodyParts entry
    std::map<int, AttachmentPoint> attachmentPoints;
    glm::vec3 pelvisBind{0.0f}; // the pelvis position the meshes were made with
    float pelvisToFoot = 0.0f;
    float bodyHeight = 0.0f;

    static std::expected<BaseAvatar, std::string> load(const std::filesystem::path& directory);
};

std::expected<BodyMesh, std::string> loadLlm(const std::filesystem::path& path);

// The attachment point an attached object uses, from its state byte.
inline int attachmentPointFromState(uint8_t state)
{
    return ((state & 0xF0) >> 4) | ((state & 0x0F) << 4);
}

} // namespace viewer::avatar
