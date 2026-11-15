#include "viewer/avatar/BaseAvatar.hpp"

#include <tinyxml2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
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

// Bones and collision volumes, in document order (parents before children).
void addBones(Skeleton& skeleton, const tinyxml2::XMLElement* element, int parent)
{
    for(const auto* bone = element->FirstChildElement(); bone; bone = bone->NextSiblingElement())
    {
        const std::string_view tag = bone->Name();
        if(tag != "bone" && tag != "collision_volume")
        {
            continue;
        }
        Joint j;
        j.name = bone->Attribute("name") ? bone->Attribute("name") : "";
        j.parent = parent;
        j.volume = tag == "collision_volume";
        j.position = parseVec3(bone->Attribute("pos"));
        j.rotation = jointRotation(parseVec3(bone->Attribute("rot")));
        j.scale = parseVec3(bone->Attribute("scale"), glm::vec3(1.0f));
        j.pivot = parseVec3(bone->Attribute("pivot"));
        j.base =
            !bone->Attribute("support") || std::string_view(bone->Attribute("support")) == "base";
        const int index = static_cast<int>(skeleton.joints.size());
        skeleton.byName[j.name] = index;
        // Other names animations may use ("hip", "avatar_mPelvis", ...).
        std::istringstream aliases(bone->Attribute("aliases") ? bone->Attribute("aliases") : "");
        for(std::string alias; aliases >> alias;)
        {
            skeleton.byName.try_emplace(alias, index);
        }
        skeleton.joints.push_back(std::move(j));
        if(tag == "bone")
        {
            addBones(skeleton, bone, index);
        }
    }
}

template <typename T>
bool readValue(std::ifstream& in, T& out)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&out), sizeof(T)));
}

float floatAttribute(const tinyxml2::XMLElement* e, const char* name, float fallback)
{
    return e->FloatAttribute(name, fallback);
}

Sex parseSex(const char* text)
{
    const std::string_view sex = text ? text : "both";
    return sex == "male" ? Sex::Male : sex == "female" ? Sex::Female : Sex::Both;
}

// Every <param> in avatar_lad.xml, recursively; the first definition of an
// id wins. `part` is the body part of the enclosing level 0 <mesh>, if any.
void collectParams(BaseAvatar& avatar, const tinyxml2::XMLElement* element,
                   std::optional<size_t> part)
{
    for(const auto* child = element->FirstChildElement(); child;
        child = child->NextSiblingElement())
    {
        const std::string_view tag = child->Name();
        if(tag == "mesh")
        {
            std::optional<size_t> meshPart;
            const char* type = child->Attribute("type");
            if(child->IntAttribute("lod") == 0 && type)
            {
                for(size_t i = 0; i < std::size(kBodyParts); i++)
                {
                    if(std::string_view(kBodyParts[i].type) == type)
                    {
                        meshPart = i;
                    }
                }
            }
            collectParams(avatar, child, meshPart);
            continue;
        }
        if(tag != "param")
        {
            collectParams(avatar, child, part);
            continue;
        }

        const int id = child->IntAttribute("id", -1);
        if(id < 0)
        {
            continue;
        }
        auto [it, inserted] = avatar.params.try_emplace(id);
        VisualParam& p = it->second;
        if(inserted)
        {
            p.id = id;
            p.group = child->IntAttribute("group", 0);
            p.name = child->Attribute("name") ? child->Attribute("name") : "";
            p.min = floatAttribute(child, "value_min", 0.0f);
            p.max = floatAttribute(child, "value_max", 1.0f);
            p.defaultWeight =
                std::clamp(floatAttribute(child, "value_default", 0.0f), p.min, p.max);
            p.sex = parseSex(child->Attribute("sex"));
        }

        if(child->FirstChildElement("param_morph") && part)
        {
            // As llpolymorph.cpp: "<name>_Driven" falls back to "<name>".
            const BodyMesh& mesh = avatar.meshes[*part];
            std::string name = child->Attribute("name") ? child->Attribute("name") : "";
            const Morph* morph = mesh.findMorph(name);
            if(!morph)
            {
                if(const size_t at = name.find("_Driven"); at != std::string::npos && at > 0)
                {
                    morph = mesh.findMorph(name.substr(0, at));
                }
            }
            if(morph)
            {
                p.morphs.push_back({*part, static_cast<size_t>(morph - mesh.morphs.data())});
                // Collision volumes the morph also resizes (fitted mesh
                // follows them).
                for(const auto* v =
                        child->FirstChildElement("param_morph")->FirstChildElement("volume_morph");
                    v; v = v->NextSiblingElement("volume_morph"))
                {
                    VisualParam::BoneDeformation d;
                    d.joint =
                        avatar.skeleton.find(v->Attribute("name") ? v->Attribute("name") : "");
                    d.scale = parseVec3(v->Attribute("scale"));
                    d.offset = parseVec3(v->Attribute("pos"));
                    if(d.joint >= 0 && avatar.skeleton.joints[static_cast<size_t>(d.joint)].volume)
                    {
                        p.volumes.push_back(d);
                    }
                }
            }
        }
        if(const auto* skeletal = child->FirstChildElement("param_skeleton"))
        {
            for(const auto* bone = skeletal->FirstChildElement("bone"); bone;
                bone = bone->NextSiblingElement("bone"))
            {
                VisualParam::BoneDeformation d;
                d.joint =
                    avatar.skeleton.find(bone->Attribute("name") ? bone->Attribute("name") : "");
                d.scale = parseVec3(bone->Attribute("scale"));
                d.offset = parseVec3(bone->Attribute("offset"));
                if(d.joint >= 0)
                {
                    p.bones.push_back(d);
                }
            }
        }
        if(const auto* driver = child->FirstChildElement("param_driver"))
        {
            for(const auto* driven = driver->FirstChildElement("driven"); driven;
                driven = driven->NextSiblingElement("driven"))
            {
                // Defaults as lldriverparam.cpp: a ramp over the driver's range.
                VisualParam::Driven d;
                d.id = driven->IntAttribute("id");
                d.min1 = floatAttribute(driven, "min1", p.min);
                d.max1 = floatAttribute(driven, "max1", p.max);
                d.max2 = floatAttribute(driven, "max2", d.max1);
                d.min2 = floatAttribute(driven, "min2", d.max1);
                p.driven.push_back(d);
            }
        }
    }
}

} // namespace

const Morph* BodyMesh::findMorph(std::string_view name) const
{
    for(const Morph& m : morphs)
    {
        if(m.name == name)
        {
            return &m;
        }
    }
    return nullptr;
}

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

    // Morph targets, until "End Morphs" (or the end of the file).
    char morphName[64] = {};
    while(in.read(morphName, 64))
    {
        Morph morph;
        morph.name.assign(morphName, strnlen(morphName, 64));
        if(morph.name == "End Morphs")
        {
            break;
        }
        int32_t count = 0;
        if(!readValue(in, count) || count < 0 || count > vertexCount)
        {
            return std::unexpected(std::format("{}: bad morph {}", path.string(), morph.name));
        }
        for(int32_t v = 0; v < count; v++)
        {
            uint32_t index = 0;
            float data[3 + 3 + 3 + 2] = {};
            readValue(in, index);
            in.read(reinterpret_cast<char*>(data), sizeof(data));
            if(!in || index >= vertexCount)
            {
                return std::unexpected(std::format("{}: bad morph {}", path.string(), morph.name));
            }
            morph.vertices.push_back(index);
            morph.positions.emplace_back(data[0], data[1], data[2]);
            morph.normals.emplace_back(data[3], data[4], data[5]);
            morph.texCoords.emplace_back(data[9], data[10]);
        }
        mesh.morphs.push_back(std::move(morph));
    }
    // Vertex remaps follow; only used for clothing masks, which we don't do.

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
        // mRoot is the avatar root itself (joint -1); mScreen holds the HUD
        // points, which aren't part of the world.
        const std::string_view joint = point->Attribute("joint") ? point->Attribute("joint") : "";
        a.hud = joint == "mScreen";
        a.visibleInFirstPerson = point->BoolAttribute("visible_in_first_person", true);
        if(a.joint >= 0 || joint == "mRoot" || a.hud)
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

    // Where each joint was when the meshes were made: the sum of its base
    // ancestors' pivots (llavatarjointmesh.cpp totalSkinOffset).
    const auto& joints = avatar.skeleton.joints;
    for(size_t i = 0; i < joints.size(); i++)
    {
        const glm::vec3 own = joints[i].base ? joints[i].pivot : glm::vec3(0.0f);
        avatar.bindPositions.push_back(
            joints[i].parent < 0
                ? own
                : avatar.bindPositions[static_cast<size_t>(joints[i].parent)] + own);
    }
    // The joints a weighted mesh's vertex weights index (llavatarjointmesh.cpp
    // setupJoint): its skin joints in skeleton order, each preceded by its
    // nearest base ancestor unless that was the previous entry.
    for(size_t m = 0; m < avatar.meshes.size(); m++)
    {
        std::vector<int> entries;
        const BodyMesh& mesh = avatar.meshes[m];
        if(mesh.weights.empty())
        {
            const int joint = kBodyParts[m].joint ? avatar.skeleton.find(kBodyParts[m].joint) : -1;
            entries.push_back(joint);
        }
        for(int j = 0; j < static_cast<int>(joints.size()) && !mesh.weights.empty(); j++)
        {
            const bool skin = std::ranges::any_of(mesh.jointNames, [&](const std::string& name)
                                                  { return avatar.skeleton.find(name) == j; });
            if(!skin)
            {
                continue;
            }
            int ancestor = joints[static_cast<size_t>(j)].parent;
            while(ancestor >= 0 && joints[static_cast<size_t>(ancestor)].parent >= 0 &&
                  !joints[static_cast<size_t>(ancestor)].base)
            {
                ancestor = joints[static_cast<size_t>(ancestor)].parent;
            }
            if(entries.empty() || entries.back() != ancestor)
            {
                entries.push_back(ancestor);
            }
            entries.push_back(j);
        }
        avatar.skinJoints.push_back(std::move(entries));
    }

    collectParams(avatar, lad.RootElement(), std::nullopt);
    for(const auto& [id, param] : avatar.params)
    {
        if(param.transmitted())
        {
            avatar.transmitted.push_back(id);
        }
    }
    return avatar;
}

} // namespace viewer::avatar
