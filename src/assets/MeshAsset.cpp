#include "viewer/assets/MeshAsset.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

namespace viewer::assets
{

namespace
{

constexpr std::string_view kDeprecatedHeader = "<? LLSD/Binary ?>";
constexpr const char* kLodNames[] = {"lowest_lod", "low_lod", "medium_lod", "high_lod"};

MeshBlock block(const llsd::Value& v)
{
    if(!v.isMap())
    {
        return {};
    }
    return MeshBlock{v["offset"].asInteger(), v["size"].asInteger()};
}

glm::vec3 vec3(const llsd::Value& v)
{
    return glm::vec3(static_cast<float>(v[0].asReal()), static_cast<float>(v[1].asReal()),
                     static_cast<float>(v[2].asReal()));
}

uint16_t u16(const llsd::Binary& b, size_t index)
{
    return static_cast<uint16_t>(b[index * 2] | b[index * 2 + 1] << 8);
}

// A 4x4 matrix as 16 numbers, in the official viewer's layout (row vectors,
// translation in 12..14). Read straight into glm's column-major storage,
// that's the same transform for column vectors.
std::optional<glm::mat4> mat4(const llsd::Value& v)
{
    if(!v.isArray() || v.size() < 16)
    {
        return std::nullopt;
    }
    glm::mat4 m(1.0f);
    for(int i = 0; i < 16; i++)
    {
        m[i / 4][i % 4] = static_cast<float>(v[static_cast<size_t>(i)].asReal());
    }
    return m;
}

} // namespace

int MeshHeader::closestLod(int wanted) const
{
    for(int distance = 0; distance < 4; distance++)
    {
        for(const int lod : {wanted + distance, wanted - distance})
        {
            if(lod >= 0 && lod < 4 && lods[static_cast<size_t>(lod)].present())
            {
                return lod;
            }
        }
    }
    return -1;
}

std::expected<MeshHeader, std::string> parseMeshHeader(std::span<const uint8_t> data)
{
    MeshHeader header;
    size_t skipped = 0;
    if(data.size() > kDeprecatedHeader.size() &&
       std::memcmp(data.data(), kDeprecatedHeader.data(), kDeprecatedHeader.size()) == 0)
    {
        // An old header line (and its newline) before the LLSD.
        skipped = kDeprecatedHeader.size() + 1;
        data = data.subspan(std::min(skipped, data.size()));
    }
    size_t consumed = 0;
    auto value = llsd::fromBinary(data, &consumed);
    if(!value)
    {
        return std::unexpected("mesh header: " + value.error());
    }
    if(!value->isMap())
    {
        return std::unexpected(std::string("mesh header isn't a map"));
    }
    header.headerSize = skipped + consumed;
    header.version = (*value)["version"].asInteger();
    for(size_t i = 0; i < 4; i++)
    {
        header.lods[i] = block((*value)[kLodNames[i]]);
    }
    header.skin = block((*value)["skin"]);
    header.physicsConvex = block((*value)["physics_convex"]);
    header.physicsMesh = block((*value)["physics_mesh"]);
    if(header.closestLod(3) < 0)
    {
        return std::unexpected(std::string("mesh header lists no levels of detail"));
    }
    return header;
}

std::expected<std::vector<uint8_t>, std::string> inflateZlib(std::span<const uint8_t> data,
                                                             size_t maxSize)
{
    z_stream stream{};
    if(inflateInit(&stream) != Z_OK)
    {
        return std::unexpected(std::string("inflateInit failed"));
    }
    std::vector<uint8_t> out;
    out.resize(std::min<size_t>(std::max<size_t>(data.size() * 4, 4096), maxSize));
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    int result = Z_OK;
    while(result != Z_STREAM_END)
    {
        if(stream.total_out >= out.size())
        {
            if(out.size() >= maxSize)
            {
                inflateEnd(&stream);
                return std::unexpected(std::string("inflated data too large"));
            }
            out.resize(std::min(out.size() * 2, maxSize));
        }
        stream.next_out = out.data() + stream.total_out;
        stream.avail_out = static_cast<uInt>(out.size() - stream.total_out);
        result = inflate(&stream, Z_NO_FLUSH);
        if(result != Z_OK && result != Z_STREAM_END)
        {
            inflateEnd(&stream);
            return std::unexpected(std::format("inflate failed ({})", result));
        }
        if(result == Z_OK && stream.avail_in == 0 && stream.avail_out > 0)
        {
            inflateEnd(&stream);
            return std::unexpected(std::string("compressed data ends early"));
        }
    }
    out.resize(stream.total_out);
    inflateEnd(&stream);
    return out;
}

std::expected<std::vector<volume::VolumeFace>, std::string>
decodeMeshLod(std::span<const uint8_t> compressed)
{
    auto inflated = inflateZlib(compressed);
    if(!inflated)
    {
        return std::unexpected("mesh LOD: " + inflated.error());
    }
    auto model = llsd::fromBinary(*inflated);
    if(!model)
    {
        return std::unexpected("mesh LOD: " + model.error());
    }
    if(!model->isArray() || model->size() == 0)
    {
        return std::unexpected(std::string("mesh LOD has no faces"));
    }

    std::vector<volume::VolumeFace> faces(model->size());
    for(size_t i = 0; i < model->size(); i++)
    {
        const llsd::Value& f = (*model)[i];
        volume::VolumeFace& face = faces[i];
        if(f.has("NoGeometry"))
        {
            continue;
        }
        const llsd::Binary positions = f["Position"].asBinary();
        const llsd::Binary normals = f["Normal"].asBinary();
        const llsd::Binary texCoords = f["TexCoord0"].asBinary();
        const llsd::Binary triangles = f["TriangleList"].asBinary();
        const size_t vertexCount = positions.size() / 6;
        size_t indexCount = triangles.size() / 2;
        indexCount -= indexCount % 3;
        if(vertexCount == 0 || indexCount < 3)
        {
            continue;
        }
        if(vertexCount > 65535)
        {
            return std::unexpected(std::string("mesh face has too many vertices"));
        }

        // Quantized to 16 bits within each face's domain.
        const glm::vec3 minPos = vec3(f["PositionDomain"]["Min"]);
        const glm::vec3 maxPos = vec3(f["PositionDomain"]["Max"]);
        const glm::vec3 posRange = maxPos - minPos;
        glm::vec2 minUv(0.0f);
        glm::vec2 maxUv(0.0f);
        if(f.has("TexCoord0Domain"))
        {
            const llsd::Value& d = f["TexCoord0Domain"];
            minUv = glm::vec2(static_cast<float>(d["Min"][0].asReal()),
                              static_cast<float>(d["Min"][1].asReal()));
            maxUv = glm::vec2(static_cast<float>(d["Max"][0].asReal()),
                              static_cast<float>(d["Max"][1].asReal()));
        }
        const glm::vec2 uvRange = maxUv - minUv;

        face.positions.resize(vertexCount);
        face.normals.resize(vertexCount, glm::vec3(0.0f, 0.0f, 1.0f));
        face.texCoords.resize(vertexCount, glm::vec2(0.0f));
        for(size_t v = 0; v < vertexCount; v++)
        {
            const glm::vec3 q(u16(positions, v * 3), u16(positions, v * 3 + 1),
                              u16(positions, v * 3 + 2));
            face.positions[v] = q / 65535.0f * posRange + minPos;
            if(normals.size() >= (v + 1) * 6)
            {
                const glm::vec3 n(u16(normals, v * 3), u16(normals, v * 3 + 1),
                                  u16(normals, v * 3 + 2));
                const glm::vec3 unit = n / 65535.0f * 2.0f - 1.0f;
                const float length = glm::length(unit);
                face.normals[v] = length > 1e-6f ? unit / length : glm::vec3(0.0f, 0.0f, 1.0f);
            }
            if(texCoords.size() >= (v + 1) * 4)
            {
                const glm::vec2 t(u16(texCoords, v * 2), u16(texCoords, v * 2 + 1));
                face.texCoords[v] = t / 65535.0f * uvRange + minUv;
            }
        }
        if(f.has("Weights"))
        {
            // Per vertex: up to four (joint byte, 16-bit weight) pairs, ended
            // early by 0xFF (llvolume.cpp unpackVolumeFaces).
            const llsd::Binary weights = f["Weights"].asBinary();
            face.joints.assign(vertexCount, glm::u8vec4(0));
            face.weights.assign(vertexCount, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
            size_t at = 0;
            for(size_t v = 0; v < vertexCount && at < weights.size(); v++)
            {
                glm::u8vec4 joints(0);
                glm::vec4 w(0.0f);
                int count = 0;
                uint8_t joint = weights[at++];
                while(joint != 0xFF && at + 2 <= weights.size())
                {
                    const uint16_t influence =
                        static_cast<uint16_t>(weights[at] | weights[at + 1] << 8);
                    at += 2;
                    joints[count] = joint;
                    w[count] = std::clamp(static_cast<float>(influence) / 65535.0f, 0.001f, 0.999f);
                    count++;
                    if(count >= 4 || at >= weights.size())
                    {
                        break;
                    }
                    joint = weights[at++];
                }
                const float sum = w.x + w.y + w.z + w.w;
                face.joints[v] = joints;
                face.weights[v] = sum > 0.0f ? w / sum : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            }
        }
        face.indices.resize(indexCount);
        for(size_t j = 0; j < indexCount; j++)
        {
            const uint16_t index = u16(triangles, j);
            if(index >= vertexCount)
            {
                return std::unexpected(std::string("mesh face index out of range"));
            }
            face.indices[j] = index;
        }
    }
    return faces;
}

std::expected<MeshSkin, std::string> decodeMeshSkin(std::span<const uint8_t> compressed)
{
    auto inflated = inflateZlib(compressed);
    if(!inflated)
    {
        return std::unexpected("mesh skin: " + inflated.error());
    }
    auto value = llsd::fromBinary(*inflated);
    if(!value || !value->isMap())
    {
        return std::unexpected(std::string("mesh skin isn't a map"));
    }
    const llsd::Value& v = *value;
    MeshSkin skin;
    const llsd::Value& names = v["joint_names"];
    for(size_t i = 0; i < names.size(); i++)
    {
        skin.jointNames.push_back(names[i].asString());
    }
    const llsd::Value& inverse = v["inverse_bind_matrix"];
    for(size_t i = 0; i < inverse.size(); i++)
    {
        skin.inverseBind.push_back(mat4(inverse[i]).value_or(glm::mat4(1.0f)));
    }
    if(skin.inverseBind.size() != skin.jointNames.size())
    {
        return std::unexpected(std::string("mesh skin: joint and bind matrix counts differ"));
    }
    if(skin.jointNames.size() > 255)
    {
        return std::unexpected(std::string("mesh skin has too many joints"));
    }
    skin.bindShape = mat4(v["bind_shape_matrix"]).value_or(glm::mat4(1.0f));
    const llsd::Value& alternate = v["alt_inverse_bind_matrix"];
    for(size_t i = 0; i < alternate.size(); i++)
    {
        skin.alternateBind.push_back(mat4(alternate[i]).value_or(glm::mat4(1.0f)));
    }
    if(skin.alternateBind.size() != skin.jointNames.size())
    {
        skin.alternateBind.clear();
    }
    if(v.has("pelvis_offset"))
    {
        skin.pelvisOffset = static_cast<float>(v["pelvis_offset"].asReal());
    }
    skin.lockScaleIfJointPosition = v["lock_scale_if_joint_position"].asBoolean();
    return skin;
}

} // namespace viewer::assets
