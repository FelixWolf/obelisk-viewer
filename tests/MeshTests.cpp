#include "viewer/assets/MeshAsset.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <zlib.h>

using namespace viewer;
namespace llsd = viewer::llsd;

namespace
{

std::vector<uint8_t> deflate(const std::vector<uint8_t>& data)
{
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> out(size);
    REQUIRE(compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), 9) == Z_OK);
    out.resize(size);
    return out;
}

llsd::Binary u16s(std::initializer_list<uint16_t> values)
{
    llsd::Binary b;
    for(const uint16_t v : values)
    {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
    return b;
}

llsd::Value vec(float a, float b, float c)
{
    llsd::Value v;
    v.append(a);
    v.append(b);
    v.append(c);
    return v;
}

} // namespace

TEST_CASE("Mesh: header and a level of detail decode")
{
    // One triangle face, quantized in a -0.5..0.5 domain.
    llsd::Value face;
    face["PositionDomain"]["Min"] = vec(-0.5f, -0.5f, -0.5f);
    face["PositionDomain"]["Max"] = vec(0.5f, 0.5f, 0.5f);
    face["Position"] = u16s({0, 0, 32768, 65535, 0, 32768, 0, 65535, 32768});
    face["Normal"] = u16s({32768, 32768, 65535, 32768, 32768, 65535, 32768, 32768, 65535});
    llsd::Value uvMin;
    uvMin.append(0.0f);
    uvMin.append(0.0f);
    llsd::Value uvMax;
    uvMax.append(2.0f);
    uvMax.append(1.0f);
    face["TexCoord0Domain"]["Min"] = uvMin;
    face["TexCoord0Domain"]["Max"] = uvMax;
    face["TexCoord0"] = u16s({0, 0, 65535, 0, 0, 65535});
    face["TriangleList"] = u16s({0, 1, 2});
    llsd::Value noGeometry;
    noGeometry["NoGeometry"] = true;
    llsd::Value model;
    model.append(face);
    model.append(noGeometry);
    const auto lod = deflate(llsd::toBinary(model));

    llsd::Value header;
    header["version"] = 1;
    header["high_lod"]["offset"] = 0;
    header["high_lod"]["size"] = static_cast<int32_t>(lod.size());
    std::vector<uint8_t> asset = llsd::toBinary(header);
    const size_t headerBytes = asset.size();
    asset.insert(asset.end(), lod.begin(), lod.end());

    const auto parsed = assets::parseMeshHeader(asset);
    REQUIRE(parsed.has_value());
    CHECK(parsed->headerSize == headerBytes);
    CHECK(parsed->lods[3].present());
    CHECK_FALSE(parsed->lods[0].present());
    CHECK(parsed->closestLod(0) == 3); // falls back to what exists

    const auto block =
        std::span(asset).subspan(parsed->headerSize + static_cast<size_t>(parsed->lods[3].offset),
                                 static_cast<size_t>(parsed->lods[3].size));
    const auto faces = assets::decodeMeshLod(block);
    REQUIRE(faces.has_value());
    REQUIRE(faces->size() == 2);
    const auto& f = (*faces)[0];
    REQUIRE(f.positions.size() == 3);
    CHECK(f.positions[0].x == doctest::Approx(-0.5f));
    CHECK(f.positions[1].x == doctest::Approx(0.5f));
    CHECK(f.positions[2].y == doctest::Approx(0.5f));
    CHECK(f.normals[0].z == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(f.texCoords[1].x == doctest::Approx(2.0f));
    CHECK(f.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK((*faces)[1].indices.empty()); // NoGeometry face keeps its slot

    // The old header line before the LLSD is skipped.
    std::vector<uint8_t> old = {'<', '?', ' ', 'L', 'L', 'S', 'D', '/', 'B',
                                'i', 'n', 'a', 'r', 'y', ' ', '?', '>', '\n'};
    old.insert(old.end(), asset.begin(), asset.end());
    CHECK(assets::parseMeshHeader(old)->headerSize == headerBytes + 18);

    // Corrupt data fails cleanly.
    CHECK_FALSE(assets::decodeMeshLod(std::span(lod).first(lod.size() / 2)).has_value());
    CHECK_FALSE(assets::parseMeshHeader(std::vector<uint8_t>{'x'}).has_value());
}

TEST_CASE("Mesh: rigged weights and the skin block decode")
{
    llsd::Value face;
    face["PositionDomain"]["Min"] = vec(-0.5f, -0.5f, -0.5f);
    face["PositionDomain"]["Max"] = vec(0.5f, 0.5f, 0.5f);
    face["Position"] = u16s({0, 0, 0, 65535, 0, 0, 0, 65535, 0});
    face["TriangleList"] = u16s({0, 1, 2});
    // Vertex 0: joint 2 alone; vertex 1: joints 0 and 1 half each; vertex
    // 2: four joints (no terminator after the fourth).
    llsd::Binary weights = {2, 0xFF, 0xFF, 0xFF};
    weights.insert(weights.end(), {0, 0xFF, 0x7F, 1, 0xFF, 0x7F, 0xFF});
    for(uint8_t j = 0; j < 4; j++)
    {
        weights.insert(weights.end(), {j, 0xFF, 0x3F});
    }
    face["Weights"] = weights;
    llsd::Value model;
    model.append(face);
    const auto faces = assets::decodeMeshLod(deflate(llsd::toBinary(model)));
    REQUIRE(faces.has_value());
    const auto& f = (*faces)[0];
    REQUIRE(f.weights.size() == 3);
    CHECK(f.joints[0].x == 2);
    CHECK(f.weights[0].x == doctest::Approx(1.0f));
    CHECK(f.joints[1] == glm::u8vec4(0, 1, 0, 0));
    CHECK(f.weights[1].x == doctest::Approx(0.5f));
    CHECK(f.weights[1].y == doctest::Approx(0.5f));
    CHECK(f.joints[2] == glm::u8vec4(0, 1, 2, 3));
    CHECK(f.weights[2].w == doctest::Approx(0.25f));

    // Matrices are in the official viewer's layout: translation in 12..14.
    auto matrix = [](float tx, float ty, float tz)
    {
        llsd::Value m;
        for(int i = 0; i < 16; i++)
        {
            const float identity = i % 5 == 0 ? 1.0f : 0.0f;
            m.append(i == 12 ? tx : i == 13 ? ty : i == 14 ? tz : identity);
        }
        return m;
    };
    llsd::Value skin;
    skin["joint_names"].append(std::string("mPelvis"));
    skin["joint_names"].append(std::string("mChest"));
    skin["inverse_bind_matrix"].append(matrix(0.0f, 0.0f, -1.0f));
    skin["inverse_bind_matrix"].append(matrix(0.0f, 0.0f, -1.5f));
    skin["bind_shape_matrix"] = matrix(0.0f, 0.0f, 1.0f);
    skin["alt_inverse_bind_matrix"].append(matrix(0.0f, 0.0f, 1.067f));
    skin["alt_inverse_bind_matrix"].append(matrix(0.0f, 0.1f, 0.2f));
    skin["lock_scale_if_joint_position"] = true;
    const auto decoded = assets::decodeMeshSkin(deflate(llsd::toBinary(skin)));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->jointNames.size() == 2);
    CHECK(decoded->jointNames[1] == "mChest");
    CHECK(decoded->inverseBind[1][3].z == doctest::Approx(-1.5f));
    const glm::vec4 p = decoded->inverseBind[0] * decoded->bindShape * glm::vec4(0, 0, 0, 1);
    CHECK(p.z == doctest::Approx(0.0f));
    REQUIRE(decoded->alternateBind.size() == 2);
    CHECK(decoded->alternateBind[1][3].y == doctest::Approx(0.1f));
    CHECK(decoded->lockScaleIfJointPosition);

    // Mismatched counts are refused.
    skin["inverse_bind_matrix"] = llsd::Value();
    CHECK_FALSE(assets::decodeMeshSkin(deflate(llsd::toBinary(skin))).has_value());
}
