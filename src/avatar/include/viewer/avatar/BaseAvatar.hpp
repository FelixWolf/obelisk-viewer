#pragma once

// The default avatar as the official viewer defines it: skeleton
// (avatar_skeleton.xml), body meshes (.llm) and attachment points
// (avatar_lad.xml). Formats follow llavatarappearance.cpp, llpolymesh.cpp and
// llvoavatar.cpp; our own implementation.
//
// Shape (visual parameters) is applied by Shape.hpp; no animation yet.

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
    glm::vec3 pivot{0.0f}; // where the body meshes were made around it
    bool base = true;      // part of the base skeleton (not "extended")
    bool volume = false;   // a collision volume (fitted mesh is skinned to them)
};

struct Skeleton
{
    std::vector<Joint> joints;                      // parents come before children
    std::map<std::string, int, std::less<>> byName; // names and aliases

    int find(std::string_view name) const;
    // Bone numbers, as the official viewer numbers joints (LLJoint
    // getJointNum): the bones in avatar_skeleton.xml's order, from mPelvis
    // (0), without the collision volumes. -1 for a volume or out of range.
    int boneNumber(int joint) const;
    int jointOfBone(int number) const;
    // Joint transforms relative to the avatar root, in the rest pose with the
    // pelvis at the root (as the official viewer places it).
    std::vector<glm::mat4> restPose() const;
};

// A morph target: offsets for some of a mesh's vertices at weight 1.
struct Morph
{
    std::string name;
    std::vector<uint32_t> vertices;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;
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
    std::vector<Morph> morphs;

    const Morph* findMorph(std::string_view name) const;
};

struct AttachmentPoint
{
    int id = 0;
    std::string name;
    int joint = -1; // -1: the avatar root
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // A HUD point (31 to 38, on the mScreen joint): in HUD space, not on the
    // body (see AvatarView::attachmentFrame).
    bool hud = false;
    // Shown in first person (avatar_lad.xml visible_in_first_person): the
    // head's points aren't.
    bool visibleInFirstPerson = true;
};

// The meshes the body is drawn with, and the baked texture each uses
// (texture entry index of the bake).
struct BodyPart
{
    const char* type; // the <mesh type> in avatar_lad.xml
    const char* file;
    int bake; // texture entry index: 8 head, 9 upper, 10 lower, 11 eyes, 19 skirt, 20 hair
    const char* bakeName; // in the appearance service URL
    const char* joint;    // for unweighted meshes: the joint they ride on
};

// clang-format off
inline constexpr BodyPart kBodyParts[] = {
    {"headMesh",         "avatar_head.llm",       8,  "head",  nullptr},
    {"eyelashMesh",      "avatar_eyelashes.llm",  8,  "head",  nullptr},
    {"upperBodyMesh",    "avatar_upper_body.llm", 9,  "upper", nullptr},
    {"lowerBodyMesh",    "avatar_lower_body.llm", 10, "lower", nullptr},
    {"hairMesh",         "avatar_hair.llm",       20, "hair",  nullptr},
    {"skirtMesh",        "avatar_skirt.llm",      19, "skirt", nullptr},
    {"eyeBallLeftMesh",  "avatar_eye.llm",        11, "eyes",  "mEyeLeft"},
    {"eyeBallRightMesh", "avatar_eye.llm",        11, "eyes",  "mEyeRight"},
};
// clang-format on

enum class Sex : uint8_t
{
    Female = 1,
    Male = 2,
    Both = 3,
};

// A <param> from avatar_lad.xml. Only the kinds that change the shape carry
// data; texture and color parameters (baked by the server) are only known
// by id so the appearance message's values line up.
struct VisualParam
{
    struct MorphRef
    {
        size_t part;  // index into kBodyParts / BaseAvatar::meshes
        size_t morph; // index into that mesh's morphs
    };
    struct BoneDeformation
    {
        int joint = -1;
        glm::vec3 scale{0.0f};  // added to the joint's scale at weight 1
        glm::vec3 offset{0.0f}; // added to its position at weight 1
    };
    // A driven parameter's weight as a function of this one's: 0 up to
    // min1, rising to 1 at max1, 1 until max2, falling to 0 at min2.
    struct Driven
    {
        int id = 0;
        float min1 = 0.0f;
        float max1 = 0.0f;
        float max2 = 0.0f;
        float min2 = 0.0f;
    };

    int id = 0;
    int group = 0; // 0 and 3 are in the appearance message
    std::string name;
    float min = 0.0f;
    float max = 1.0f;
    float defaultWeight = 0.0f;
    Sex sex = Sex::Both;
    std::vector<MorphRef> morphs;
    std::vector<BoneDeformation> bones;
    std::vector<BoneDeformation> volumes; // collision volumes a morph resizes
    std::vector<Driven> driven;
    // Its morph follows clothing (avatar_lad.xml <morph_masks>): weighed per
    // vertex by this bake's morph mask (texture entry index; -1: none),
    // inverted if so said (llpolymorph.cpp LLPolyVertexMask).
    int maskBake = -1;
    bool maskInvert = false;

    bool transmitted() const { return group == 0 || group == 3; }
};

struct BaseAvatar
{
    Skeleton skeleton;
    std::vector<BodyMesh> meshes; // one per kBodyParts entry
    std::map<int, AttachmentPoint> attachmentPoints;
    // Per joint: where it was when the body meshes were made.
    std::vector<glm::vec3> bindPositions;
    // Per body part: the joints its vertex weights index (-1: the avatar
    // root); for unweighted meshes, the one joint they ride on.
    std::vector<std::vector<int>> skinJoints;
    std::map<int, VisualParam> params;
    std::vector<int> transmitted; // ids in the appearance message's order
    glm::vec3 pelvisBind{0.0f};   // the pelvis position the meshes were made with
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
