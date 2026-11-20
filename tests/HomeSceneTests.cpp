#include "viewer/llsd/Serialization.hpp"
#include "viewer/world/HomeScene.hpp"

#include <doctest/doctest.h>

#include <cstring>

using namespace viewer;

namespace
{

constexpr uint64_t kRegion = (uint64_t{256000} << 32) | uint64_t{256000};

llsd::Value array(std::initializer_list<double> values)
{
    llsd::Value a{llsd::Array{}};
    for(const double v : values)
    {
        a.append(llsd::Value(v));
    }
    return a;
}

llsd::Value prim(const char* name, glm::vec3 pos, glm::vec4 rotation)
{
    llsd::Value p{llsd::Map{}};
    p["name"] = llsd::Value(std::string(name));
    p["pos"] = array({pos.x, pos.y, pos.z});
    p["rotation"] = array({rotation.x, rotation.y, rotation.z, rotation.w});
    p["scale"] = array({1.0, 2.0, 3.0});
    llsd::Value shape{llsd::Map{}};
    shape["path"]["curve"] = llsd::Value(0x10);
    shape["path"]["end"] = llsd::Value(1.0);
    shape["path"]["scale_x"] = llsd::Value(1.0);
    shape["path"]["scale_y"] = llsd::Value(1.0);
    shape["path"]["revolutions"] = llsd::Value(1.0);
    shape["profile"]["curve"] = llsd::Value(0x01);
    shape["profile"]["end"] = llsd::Value(1.0);
    shape["profile"]["hollow"] = llsd::Value(0.5);
    p["shape"] = shape;
    return p;
}

// A scene: terrain at 3 m (one sample at 7), water at 2, a marker of each
// kind and one two-prim linkset.
std::vector<uint8_t> scene()
{
    llsd::Value root{llsd::Map{}};
    root["_version"] = llsd::Value(1);
    root["asset_cdn"] = llsd::Value(std::string("assets.example/"));
    llsd::Value& terrain = root["scene"]["terrain"];
    terrain["water"] = llsd::Value(2.0);
    std::vector<float> heights(256 * 256, 3.0f);
    heights[17 * 256 + 33] = 7.0f;
    llsd::Binary bytes(heights.size() * sizeof(float));
    std::memcpy(bytes.data(), heights.data(), bytes.size()); // little-endian hosts
    terrain["heights"] = llsd::Value(std::move(bytes));
    terrain["textures"]["detail"] = llsd::Value(llsd::Array{});
    for(int i = 0; i < 4; i++)
    {
        terrain["textures"]["detail"].append(llsd::Value(Uuid::random()));
    }
    terrain["textures"]["startHeights"] = array({1, 2, 3, 4});
    terrain["textures"]["heightRanges"] = array({5, 6, 7, 8});

    llsd::Value objects{llsd::Array{}};
    auto single = [&](llsd::Value p)
    {
        llsd::Value linkset{llsd::Array{}};
        linkset.append(std::move(p));
        objects.append(std::move(linkset));
    };
    single(prim("#HOME:Floor", {10, 20, 3.5f}, {0, 0, 0, 1}));
    single(prim("#HOME:Landing", {10, 20, 3.6f}, {0, 0, 0, 1}));
    single(prim("#HOME:Login", {10, 22, 4.5f}, {0, 0, 1, 0})); // turned round: faces -Y
    single(prim("#HOME:News", {11, 22, 4.5f}, {0, 0, 0, 1}));
    single(prim("#HOME:GridStatus", {9, 22, 4.5f}, {0, 0, 0, 1}));

    llsd::Value rootPrim = prim("Table", {50, 60, 4}, {0, 0, 0, 1});
    llsd::Value face{llsd::Map{}};
    face["imageid"] = llsd::Value(*Uuid::parse("11111111-2222-3333-4444-555555555555"));
    face["color"] = array({1, 0.5, 0.25, 1});
    face["scales"] = llsd::Value(2.0);
    face["scalet"] = llsd::Value(3.0);
    face["bump"] = llsd::Value(0x41); // shiny low, bump 1
    face["fullbright"] = llsd::Value(1);
    face["material"]["normMap"] = llsd::Value(Uuid::random());
    face["material"]["normRepeatX"] = llsd::Value(20000);
    face["material"]["diffuseAlphaMode"] = llsd::Value(2);
    face["gltfMaterial"]["rf"] = llsd::Value(0.25);
    rootPrim["facelist"] = llsd::Value(llsd::Array{});
    rootPrim["facelist"].append(face);
    rootPrim["extra_parameters"]["light"]["color"] = array({1, 0, 0, 0.5});
    rootPrim["extra_parameters"]["light"]["radius"] = llsd::Value(4.0);
    rootPrim["particles"]["pscrc"] = llsd::Value(1);
    rootPrim["particles"]["psburstpartcount"] = llsd::Value(7);
    llsd::Value linkset{llsd::Array{}};
    linkset.append(rootPrim);
    linkset.append(prim("Leg", {0, 0, -1}, {0, 0, 0, 1}));
    objects.append(linkset);
    root["scene"]["objects"] = objects;
    return llsd::toBinary(root);
}

} // namespace

TEST_CASE("a home scene: markers, terrain and objects")
{
    const auto loaded = world::loadHomeScene(scene(), kRegion);
    REQUIRE(loaded);
    const world::HomeScene& s = *loaded;
    CHECK(s.assetUrl == "http://assets.example");

    // The markers: places, not drawn.
    REQUIRE(s.floor);
    CHECK(*s.floor == doctest::Approx(3.5f));
    REQUIRE(s.landing);
    CHECK(s.landing->position.y == doctest::Approx(20.0f));
    REQUIRE(s.login);
    const glm::vec3 forward = s.login->rotation * glm::vec3(0, 1, 0);
    CHECK(forward.y == doctest::Approx(-1.0f));
    CHECK(s.news);
    CHECK(s.gridStatus);

    // A world made of it.
    world::World w;
    for(const auto& u : s.updates)
    {
        w.apply(u);
    }
    REQUIRE(w.objectCount() == 2);
    const world::RegionTerrain* terrain = w.terrain(kRegion);
    REQUIRE(terrain);
    CHECK(terrain->complete());
    CHECK(terrain->height(33, 17) == 7.0f);
    CHECK(terrain->height(0, 0) == 3.0f);
    CHECK(terrain->composition.waterHeight == 2.0f);
    CHECK(terrain->composition.heightRange[3] == 8.0f);

    const world::Object* table = w.object(kRegion, 1);
    REQUIRE(table);
    CHECK(table->parentId == 0);
    CHECK(table->position == glm::vec3(50, 60, 4));
    CHECK(table->scale == glm::vec3(1, 2, 3));
    CHECK(table->volume.profile.curve == 0x01);
    CHECK(table->volume.profile.hollow == doctest::Approx(0.5f));
    const world::TextureFace& f = table->faces[0];
    CHECK(f.texture.toString() == "11111111-2222-3333-4444-555555555555");
    CHECK(f.repeats == glm::vec2(2, 3));
    CHECK(f.bump() == 1);
    CHECK(f.shiny() == 1);
    CHECK(f.fullbright());
    // Faces past the list are as the first.
    CHECK(table->faces[5].texture == f.texture);
    REQUIRE(table->light);
    CHECK(table->light->intensity == doctest::Approx(0.5f));
    CHECK(table->light->radius == 4.0f);
    CHECK_FALSE(table->lightImage);
    REQUIRE(table->particles);
    CHECK(table->particles->burstCount == 7);

    // The legacy material, by an id of its own.
    REQUIRE(s.legacyMaterials.size() == 1);
    CHECK(f.material == s.legacyMaterials[0].first);
    CHECK(s.legacyMaterials[0].second.normalTransform.scale.x == doctest::Approx(2.0f));
    CHECK(s.legacyMaterials[0].second.alphaMode == assets::DiffuseAlphaMode::Mask);

    // The PBR overrides, over the blank material.
    REQUIRE(table->materials.size() == 1);
    CHECK(table->materials[0].second == world::kHomeBlankMaterial);
    const world::FaceOverrides* overrides = w.materialOverrides({kRegion, 1});
    REQUIRE(overrides);
    REQUIRE(overrides->size() == 1);
    CHECK(overrides->front().second.roughness == doctest::Approx(0.25f));

    const world::Object* leg = w.object(kRegion, 2);
    REQUIRE(leg);
    CHECK(leg->parentId == 1);
    CHECK(leg->position == glm::vec3(0, 0, -1));
}

TEST_CASE("a home scene that isn't one")
{
    const std::vector<uint8_t> junk{'x', 'y', 'z'};
    CHECK_FALSE(world::loadHomeScene(junk, kRegion));
    llsd::Value wrong{llsd::Map{}};
    wrong["_version"] = llsd::Value(2);
    CHECK_FALSE(world::loadHomeScene(llsd::toBinary(wrong), kRegion));
}
