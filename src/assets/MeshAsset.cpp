#include "viewer/assets/MeshAsset.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <zlib.h>

#include <cstring>
#include <format>

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

} // namespace viewer::assets
