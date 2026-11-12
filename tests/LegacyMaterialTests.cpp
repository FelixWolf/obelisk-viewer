#include "viewer/assets/LegacyMaterial.hpp"
#include "viewer/assets/MeshAsset.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <cstring>

using namespace viewer::assets;
namespace llsd = viewer::llsd;

namespace
{

llsd::Value materialLlsd()
{
    llsd::Value m;
    m["NormMap"] = *viewer::Uuid::parse("11111111-0000-0000-0000-000000000001");
    m["NormOffsetX"] = 2500;
    m["NormOffsetY"] = -5000;
    m["NormRepeatX"] = 20000;
    m["NormRepeatY"] = 10000;
    m["NormRotation"] = 15708;
    m["SpecMap"] = *viewer::Uuid::parse("11111111-0000-0000-0000-000000000002");
    m["SpecOffsetX"] = 0;
    m["SpecOffsetY"] = 0;
    m["SpecRepeatX"] = 10000;
    m["SpecRepeatY"] = 10000;
    m["SpecRotation"] = 0;
    llsd::Value color = llsd::Value(llsd::Array{});
    for(const int c : {255, 128, 0, 255})
    {
        color.append(c);
    }
    m["SpecColor"] = color;
    m["SpecExp"] = 51;
    m["EnvIntensity"] = 0;
    m["DiffuseAlphaMode"] = 3;
    m["AlphaMaskCutoff"] = 0;
    return m;
}

} // namespace

TEST_CASE("Legacy materials: fields, fixed point, alpha mode")
{
    const LegacyMaterial m = legacyMaterialFromLlsd(materialLlsd());
    CHECK(m.normalMap.toString() == "11111111-0000-0000-0000-000000000001");
    CHECK(m.normalTransform.offset.x == doctest::Approx(0.25f));
    CHECK(m.normalTransform.offset.y == doctest::Approx(-0.5f));
    CHECK(m.normalTransform.scale.x == doctest::Approx(2.0f));
    CHECK(m.normalTransform.rotation == doctest::Approx(1.5708f));
    CHECK(m.specularColor.g == doctest::Approx(128.0f / 255.0f));
    CHECK(m.glossiness == doctest::Approx(0.2f));
    CHECK(m.alphaMode == DiffuseAlphaMode::Emissive);
}

TEST_CASE("Legacy materials: request and answer are zipped binary LLSD")
{
    const viewer::Uuid id = *viewer::Uuid::parse("01234567-89ab-cdef-0123-456789abcdef");
    const llsd::Value request = legacyMaterialRequest(std::span(&id, 1));
    auto inflated = inflateZlib(request["Zipped"].asBinary());
    REQUIRE(inflated.has_value());
    auto ids = llsd::fromBinary(*inflated);
    REQUIRE(ids.has_value());
    REQUIRE(ids->size() == 1);
    const llsd::Binary raw = (*ids)[size_t{0}].asBinary();
    REQUIRE(raw.size() == 16);
    CHECK(std::memcmp(raw.data(), id.bytes.data(), 16) == 0);

    // The answer, with the binary header some servers put first.
    llsd::Value entry;
    entry["ID"] = llsd::Value(llsd::Binary(id.bytes.begin(), id.bytes.end()));
    entry["Material"] = materialLlsd();
    llsd::Value list = llsd::Value(llsd::Array{});
    list.append(entry);
    std::string header = "<? LLSD/Binary ?>\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    const auto body = llsd::toBinary(list);
    bytes.insert(bytes.end(), body.begin(), body.end());
    llsd::Value response;
    response["Zipped"] = llsd::Value(deflateZlib(bytes));
    auto parsed = parseLegacyMaterials(response);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    CHECK((*parsed)[0].first == id);
    CHECK((*parsed)[0].second.alphaMode == DiffuseAlphaMode::Emissive);
}
