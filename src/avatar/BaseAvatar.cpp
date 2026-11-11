#include "viewer/avatar/BaseAvatar.hpp"

#include <tinyxml2.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

namespace viewer::avatar
{

namespace
{

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

glm::vec3 parseVec3(const char* text, glm::vec3 fallback = glm::vec3(0.0f))
{
    if(!text)
    {
        return fallback;
    }
    glm::vec3 v = fallback;
    std::sscanf(text, "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

// The official viewer's mayaQ(x, y, z, XYZ): x applied first, then y, then z.
glm::quat jointRotation(const glm::vec3& degrees)
{
    const glm::quat qx = glm::angleAxis(degrees.x * kDegToRad, glm::vec3(1, 0, 0));
    const glm::quat qy = glm::angleAxis(degrees.y * kDegToRad, glm::vec3(0, 1, 0));
    const glm::quat qz = glm::angleAxis(degrees.z * kDegToRad, glm::vec3(0, 0, 1));
    return qz * qy * qx;
}

// LLQuaternion::setQuat(roll, pitch, yaw), used for attachment points:
// the Hamilton product qx * qy * qz.
glm::quat attachmentRotation(const glm::vec3& degrees)
{
    const glm::quat qx = glm::angleAxis(degrees.x * kDegToRad, glm::vec3(1, 0, 0));
    const glm::quat qy = glm::angleAxis(degrees.y * kDegToRad, glm::vec3(0, 1, 0));
    const glm::quat qz = glm::angleAxis(degrees.z * kDegToRad, glm::vec3(0, 0, 1));
    return qx * qy * qz;
}

void addBones(Skeleton& skeleton, const tinyxml2::XMLElement* element, int parent)
{
    for(const auto* bone = element->FirstChildElement("bone"); bone;
        bone = bone->NextSiblingElement("bone"))
    {
        Joint j;
        j.name = bone->Attribute("name") ? bone->Attribute("name") : "";
        j.parent = parent;
        j.position = parseVec3(bone->Attribute("pos"));
        j.rotation = jointRotation(parseVec3(bone->Attribute("rot")));
        j.scale = parseVec3(bone->Attribute("scale"), glm::vec3(1.0f));
        const int index = static_cast<int>(skeleton.joints.size());
        skeleton.byName[j.name] = index;
        skeleton.joints.push_back(std::move(j));
        addBones(skeleton, bone, index);
    }
}

template <typename T>
bool readValue(std::ifstream& in, T& out)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&out), sizeof(T)));
}

} // namespace

int Skeleton::find(std::string_view name) const
{
    const auto it = byName.find(name);
    return it == byName.end() ? -1 : it->second;
}

std::vector<glm::mat4> Skeleton::restPose() const
{
    std::vector<glm::mat4> world(joints.size());
    for(size_t i = 0; i < joints.size(); i++)
    {
        const Joint& j = joints[i];
        // The official viewer moves the pelvis to the root (llavatarappearance.cpp).
        const glm::vec3 position = j.name == "mPelvis" ? glm::vec3(0.0f) : j.position;
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(j.rotation);
        world[i] = j.parent < 0 ? local : world[static_cast<size_t>(j.parent)] * local;
    }
    return world;
}

std::expected<BodyMesh, std::string> loadLlm(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return std::unexpected(std::format("can't open {}", path.string()));
    }
    char header[24] = {};
    in.read(header, sizeof(header));
    if(std::strncmp(header, "Linden Binary Mesh 1.0", 22) != 0)
    {
        return std::unexpected(std::format("{} isn't a binary avatar mesh", path.string()));
    }
    uint8_t hasWeights = 0;
    uint8_t hasDetail = 0;
    float transform[3 + 3 + 3] = {};
    uint8_t rotationOrder = 0;
    readValue(in, hasWeights);
    readValue(in, hasDetail);
    in.read(reinterpret_cast<char*>(transform), 6 * sizeof(float));
    readValue(in, rotationOrder);
    in.read(reinterpret_cast<char*>(transform + 6), 3 * sizeof(float));

    uint16_t vertexCount = 0;
    readValue(in, vertexCount);
    BodyMesh mesh;
    auto readVec3s = [&](std::vector<glm::vec3>& out)
    {
        out.resize(vertexCount);
        in.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(vertexCount * 12));
    };
    readVec3s(mesh.positions);
    readVec3s(mesh.normals);
    std::vector<glm::vec3> binormals;
    readVec3s(binormals);
    mesh.texCoords.resize(vertexCount);
    in.read(reinterpret_cast<char*>(mesh.texCoords.data()),
            static_cast<std::streamsize>(vertexCount * 8));
    if(hasDetail)
    {
        in.seekg(vertexCount * 8, std::ios::cur);
    }
    if(hasWeights)
    {
        mesh.weights.resize(vertexCount);
        in.read(reinterpret_cast<char*>(mesh.weights.data()),
                static_cast<std::streamsize>(vertexCount * 4));
    }
    uint16_t faceCount = 0;
    readValue(in, faceCount);
    mesh.indices.resize(static_cast<size_t>(faceCount) * 3);
    for(uint32_t& index : mesh.indices)
    {
        uint16_t v = 0;
        readValue(in, v);
        index = v;
    }
    if(hasWeights)
    {
        uint16_t jointCount = 0;
        readValue(in, jointCount);
        for(uint16_t i = 0; i < jointCount; i++)
        {
            char name[64] = {};
            in.read(name, 64);
            mesh.jointNames.emplace_back(name, strnlen(name, 64));
        }
    }
    if(!in)
    {
        return std::unexpected(std::format("{} is truncated", path.string()));
    }
    for(const uint32_t index : mesh.indices)
    {
        if(index >= vertexCount)
        {
            return std::unexpected(std::format("{} has a bad index", path.string()));
        }
    }
    for(glm::vec3& n : mesh.normals)
    {
        const float length = glm::length(n);
        n = length > 1e-6f ? n / length : glm::vec3(0, 0, 1);
    }
    return mesh;
}

std::expected<BaseAvatar, std::string> BaseAvatar::load(const std::filesystem::path& directory)
{
    BaseAvatar avatar;

    tinyxml2::XMLDocument skeletonXml;
    if(skeletonXml.LoadFile((directory / "avatar_skeleton.xml").string().c_str()) !=
       tinyxml2::XML_SUCCESS)
    {
        return std::unexpected(std::string("can't read avatar_skeleton.xml"));
    }
    addBones(avatar.skeleton, skeletonXml.RootElement(), -1);
    const int pelvis = avatar.skeleton.find("mPelvis");
    if(pelvis < 0)
    {
        return std::unexpected(std::string("the skeleton has no pelvis"));
    }
    avatar.pelvisBind = avatar.skeleton.joints[static_cast<size_t>(pelvis)].position;

    // Body size, as llavatarappearance.cpp computeBodySize (default scales).
    auto pos = [&](const char* name)
    {
        const int i = avatar.skeleton.find(name);
        return i < 0 ? glm::vec3(0.0f) : avatar.skeleton.joints[static_cast<size_t>(i)].position;
    };
    avatar.pelvisToFoot =
        pos("mHipLeft").z - pos("mKneeLeft").z - pos("mAnkleLeft").z - pos("mFootLeft").z;
    avatar.bodyHeight = avatar.pelvisToFoot + std::sqrt(2.0f) * pos("mSkull").z + pos("mHead").z +
                        pos("mNeck").z + pos("mChest").z + pos("mTorso").z;

    tinyxml2::XMLDocument lad;
    if(lad.LoadFile((directory / "avatar_lad.xml").string().c_str()) != tinyxml2::XML_SUCCESS)
    {
        return std::unexpected(std::string("can't read avatar_lad.xml"));
    }
    const auto* skeletonElement = lad.RootElement()->FirstChildElement("skeleton");
    for(const auto* point = skeletonElement ? skeletonElement->FirstChildElement("attachment_point")
                                            : nullptr;
        point; point = point->NextSiblingElement("attachment_point"))
    {
        AttachmentPoint a;
        a.id = point->IntAttribute("id");
        a.name = point->Attribute("name") ? point->Attribute("name") : "";
        a.joint = avatar.skeleton.find(point->Attribute("joint") ? point->Attribute("joint") : "");
        a.position = parseVec3(point->Attribute("position"));
        a.rotation = attachmentRotation(parseVec3(point->Attribute("rotation")));
        // mRoot is the avatar root itself (joint -1); mScreen is for HUD
        // attachments, which aren't part of the world.
        const std::string_view joint = point->Attribute("joint") ? point->Attribute("joint") : "";
        if(a.joint >= 0 || joint == "mRoot")
        {
            avatar.attachmentPoints[a.id] = a;
        }
    }

    for(const BodyPart& part : kBodyParts)
    {
        auto mesh = loadLlm(directory / part.file);
        if(!mesh)
        {
            return std::unexpected(mesh.error());
        }
        avatar.meshes.push_back(std::move(*mesh));
    }
    return avatar;
}

} // namespace viewer::avatar
