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
