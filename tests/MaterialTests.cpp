#include "viewer/assets/Material.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

using namespace viewer::assets;
namespace llsd = viewer::llsd;

namespace
{

// As the official viewer writes them (tinygltf): textures by index, image
// URIs holding asset ids.
constexpr std::string_view kGltf = R"({"asset":{"version":"2.0"},
  "images":[{"uri":"11111111-0000-0000-0000-000000000001"},
            {"uri":"11111111-0000-0000-0000-000000000002"}],
  "textures":[{"source":0},{"source":1}],
  "materials":[{
    "pbrMetallicRoughness":{
      "baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":
        {"offset":[0.5,0.0],"scale":[2.0,2.0],"rotation":0.5}}},
      "baseColorFactor":[1.0,0.5,0.25,0.75],
      "metallicFactor":0.0,"roughnessFactor":0.5},
    "normalTexture":{"index":1},
    "emissiveFactor":[0.1,0.2,0.3],
    "alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true}]})";

} // namespace

TEST_CASE("PBR materials: glTF JSON and the asset wrapper")
{
    const auto m = parseGltfMaterial(kGltf);
    REQUIRE(m.has_value());
    CHECK(m->textures[kBaseColorTexture].toString() == "11111111-0000-0000-0000-000000000001");
    CHECK(m->textures[kNormalTexture].toString() == "11111111-0000-0000-0000-000000000002");
    CHECK(m->textures[kEmissiveTexture].isNull());
    CHECK(m->transforms[kBaseColorTexture].offset == glm::vec2(0.5f, 0.0f));
    CHECK(m->transforms[kBaseColorTexture].scale == glm::vec2(2.0f));
    CHECK(m->transforms[kBaseColorTexture].rotation == 0.5f);
    CHECK(m->transforms[kNormalTexture].scale == glm::vec2(1.0f));
    CHECK(m->baseColor == glm::vec4(1.0f, 0.5f, 0.25f, 0.75f));
    CHECK(m->emissive.z == doctest::Approx(0.3f));
    CHECK(m->metallic == 0.0f);
    CHECK(m->roughness == 0.5f);
    CHECK(m->alphaMode == MaterialAlpha::Mask);
    CHECK(m->alphaCutoff == doctest::Approx(0.3f));
    CHECK(m->doubleSided);

    // glTF defaults.
    const auto plain = parseGltfMaterial(R"({"materials":[{}]})");
    REQUIRE(plain.has_value());
    CHECK(plain->metallic == 1.0f);
    CHECK(plain->roughness == 1.0f);
    CHECK(plain->alphaMode == MaterialAlpha::Opaque);
    CHECK_FALSE(parseGltfMaterial(R"({"materials":[]})").has_value());

    // The asset: LLSD around the JSON.
    llsd::Value asset;
    asset["version"] = "1.1";
    asset["type"] = "GLTF 2.0";
    asset["data"] = std::string(kGltf);
    const std::string xml = llsd::toXml(asset);
    const auto fromXml =
        parseMaterialAsset(std::span(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()));
    REQUIRE(fromXml.has_value());
    CHECK(*fromXml == *m);
    const auto binary = llsd::toBinary(asset);
    CHECK(parseMaterialAsset(binary).has_value());
    asset["version"] = "9.9";
    CHECK_FALSE(parseMaterialAsset(llsd::toBinary(asset)).has_value());
}

TEST_CASE("PBR materials: overrides replace only what they set")
{
    const Material base = *parseGltfMaterial(kGltf);
    const auto data = llsd::fromNotation(
        "{'bc':[r0,r1,r0,r1],'am':i1,'ds':0,'mf':r0.25,"
        "'tex':[!,uffffffff-ffff-ffff-ffff-ffffffffffff,!,u22222222-0000-0000-0000-000000000003],"
        "'ti':[{'s':[r4,r4]},{},{},{'r':r1.5}]}");
    REQUIRE(data.has_value());
    const Material m = applyOverride(base, MaterialOverride::fromLlsd(*data));
    CHECK(m.baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    CHECK(m.alphaMode == MaterialAlpha::Blend);
    CHECK_FALSE(m.doubleSided);
    CHECK(m.metallic == 0.25f);
    CHECK(m.roughness == 0.5f); // not set: kept
    CHECK(m.textures[kBaseColorTexture] == base.textures[kBaseColorTexture]);
    CHECK(m.textures[kNormalTexture].isNull()); // the "none" id
    CHECK(m.textures[kEmissiveTexture].toString() == "22222222-0000-0000-0000-000000000003");
    CHECK(m.transforms[kBaseColorTexture].scale == glm::vec2(4.0f));
    CHECK(m.transforms[kBaseColorTexture].offset == glm::vec2(0.5f, 0.0f)); // kept
    CHECK(m.transforms[kEmissiveTexture].rotation == 1.5f);

    // An empty override changes nothing.
    CHECK(applyOverride(base, MaterialOverride::fromLlsd(llsd::Value(llsd::Map{}))) == base);
}
