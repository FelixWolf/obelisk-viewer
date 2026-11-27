#include "AvatarComplexity.hpp"
#include "AvatarRenderInfo.hpp"

#include "viewer/volume/Volume.hpp"

#include <doctest/doctest.h>

using namespace viewer;

namespace
{

constexpr uint64_t kRegion = 1000ull * 256 << 32 | 1000 * 256;

world::WorldUpdate object(uint32_t localId, uint32_t parentId, uint8_t type,
                          const std::function<void(world::Object&)>& change = {})
{
    world::ObjectUpdateData u;
    u.localId = localId;
    u.full.id.bytes[15] = static_cast<uint8_t>(localId);
    u.full.id.bytes[14] = static_cast<uint8_t>(localId >> 8);
    u.full.localId = localId;
    u.full.parentId = parentId;
    u.full.type = type;
    u.full.scale = glm::vec3(0.5f);
    u.full.hasVolume = type == world::ObjectType::Prim;
    if(parentId != 0 && type == world::ObjectType::Prim)
    {
        u.full.state = 0x20; // attachment point 2 (skull), as the state byte has it
    }
    if(change)
    {
        change(u.full);
    }
    world::WorldUpdate::Objects objects;
    objects.updates.push_back(u);
    return world::WorldUpdate{.region = kRegion, .change = std::move(objects)};
}

Uuid idOf(uint32_t localId)
{
    Uuid id;
    id.bytes[15] = static_cast<uint8_t>(localId);
    id.bytes[14] = static_cast<uint8_t>(localId >> 8);
    return id;
}

ComplexitySources noSources()
{
    return {};
}

world::Object box()
{
    world::Object o;
    o.type = world::ObjectType::Prim;
    o.hasVolume = true;
    o.scale = glm::vec3(0.5f);
    return o;
}

} // namespace

TEST_CASE("Complexity: mesh triangle estimates as LLMeshCostData")
{
    // (16384 - 384) / 16 = 1000; missing levels take the next higher one.
    const auto t = complexity::estimatedTriangles({0, 4480, 0, 16384});
    CHECK(t[3] == doctest::Approx(1000.0f));
    CHECK(t[2] == doctest::Approx(1000.0f));
    CHECK(t[1] == doctest::Approx(256.0f));
    CHECK(t[0] == doctest::Approx(256.0f));
    // Tiny levels are at least 16 bytes' worth.
    CHECK(complexity::estimatedTriangles({1, 1, 1, 1})[0] == doctest::Approx(1.0f));

    // The same everywhere: the weighting changes nothing.
    CHECK(complexity::radiusWeightedTriangles({500, 500, 500, 500}, 2.0f) ==
          doctest::Approx(500.0f));
    // Small things are mostly seen at their lowest detail, big ones at their highest.
    const std::array<float, 4> lods{10, 100, 1000, 10000};
    CHECK(complexity::radiusWeightedTriangles(lods, 0.05f) < 200.0f);
    CHECK(complexity::radiusWeightedTriangles(lods, 50.0f) > 9000.0f);

    // Each lower level may have half the one above; the rest is charged.
    CHECK(complexity::streamingTriangles({1000, 1000, 1000, 1000}) ==
          doctest::Approx(1000.0f + 500.0f + 750.0f + 875.0f));
    CHECK(complexity::streamingTriangles({125, 250, 500, 1000}) == doctest::Approx(1000.0f));
}

TEST_CASE("Complexity: texture costs by size")
{
    CHECK(complexity::textureCost({0, 0}) == 256);
    CHECK(complexity::textureCost({1024, 1024}) == 256 + 16 * 16);
    CHECK(complexity::textureCost({512, 128}) == 256 + 16 * 5);
}

TEST_CASE("Complexity: a prim's cost and its multipliers")
{
    const auto counts = volume::lodTriangleCounts(world::Object{}.volume);
    CHECK(counts[0] > 0);
    CHECK(counts[0] == counts[3]); // a box doesn't get more detailed

    std::unordered_set<Uuid> textures;
    bool pending = false;
    const ComplexitySources none = noSources();
    world::Object plain = box();
    const float base = complexity::primCost(plain, false, false, none, textures, pending);
    CHECK(base >= 2.0f);
    CHECK_FALSE(pending);
    CHECK(textures.empty()); // no textures set

    world::Object glowing = plain;
    glowing.faces[0].glow = 0.2f;
    CHECK(complexity::primCost(glowing, false, false, none, textures, pending) ==
          doctest::Approx(base * 1.5f));

    world::Object seeThrough = plain;
    seeThrough.faces[1].color.a = 0.5f;
    seeThrough.flexible = world::FlexibleParams{};
    CHECK(complexity::primCost(seeThrough, false, false, none, textures, pending) ==
          doctest::Approx(base * 4.0f * 5.0f));

    world::Object lit = plain;
    lit.light = world::LightParams{};
    lit.faces[2].mediaTexGen = 0x01; // media
    CHECK(complexity::primCost(lit, false, false, none, textures, pending) ==
          doctest::Approx(base + 500.0f + 1500.0f));

    // A mesh whose header hasn't come: nothing yet, and said so.
    world::Object mesh = plain;
    mesh.sculptType = world::SculptType::Mesh;
    mesh.sculptTexture = idOf(77);
    CHECK(complexity::primCost(mesh, false, false, none, textures, pending) == 0.0f);
    CHECK(pending);

    // Once it has: by its triangles; rigged mesh costs more.
    ComplexitySources sources;
    sources.meshLodSizes = [](const Uuid&)
    { return std::optional(std::array<int64_t, 4>{0, 0, 0, 16384}); };
    pending = false;
    const float unrigged = complexity::primCost(mesh, false, false, sources, textures, pending);
    CHECK_FALSE(pending);
    CHECK(unrigged == doctest::Approx(1000.0f * 5.0f));
    sources.meshRigged = [](const Uuid&) { return std::optional(true); };
    CHECK(complexity::primCost(mesh, false, false, sources, textures, pending) ==
          doctest::Approx(1000.0f * 5.0f * 1.2f));
}

TEST_CASE("Complexity: an attachment counts each texture once, and is held to a million")
{
    world::Object root = box();
    root.faces[0].texture = idOf(500);
    root.faces[1].texture = idOf(500);
    world::Object child = box();
    child.faces[0].texture = idOf(500);
    child.faces[1].texture = idOf(501);
    ComplexitySources sources;
    sources.textureSize = [](const Uuid& id)
    { return id == idOf(500) ? std::optional(glm::uvec2(1024, 1024)) : std::nullopt; };
    bool pending = false;
    std::unordered_set<Uuid> textures;
    const float prims = complexity::primCost(root, false, false, sources, textures, pending) +
                        complexity::primCost(child, false, false, sources, textures, pending);
    const world::Object* children[] = {&child};
    CHECK(complexity::attachmentCost(root, children, sources, pending) ==
          static_cast<uint32_t>(prims + 512.0f + 256.0f));

    // A huge mesh is held to a million.
    world::Object heavy = root;
    sources.meshLodSizes = [](const Uuid&)
    { return std::optional(std::array<int64_t, 4>{0, 0, 0, int64_t{1} << 30}); };
    heavy.sculptType = world::SculptType::Mesh;
    heavy.sculptTexture = idOf(9);
    heavy.scale = glm::vec3(50.0f);
    CHECK(complexity::attachmentCost(heavy, {}, sources, pending) == 1000000u);
}

TEST_CASE("Complexity: avatars over the limit are jelly dolls")
{
    world::World w;
    w.apply(object(1, 0, world::ObjectType::Avatar)); // someone
    w.apply(object(2, 0, world::ObjectType::Avatar)); // us
    w.apply(object(3, 0, world::ObjectType::Avatar)); // a friend
    // Something light on each, and a lot of lights on the first.
    for(uint32_t avatar : {1u, 2u, 3u})
    {
        w.apply(object(10 + avatar, avatar, world::ObjectType::Prim));
    }
    for(uint32_t i = 0; i < 20; i++)
    {
        w.apply(object(100 + i, 1, world::ObjectType::Prim,
                       [](world::Object& o) { o.light = world::LightParams{}; }));
    }
    // A HUD (point 31, the state byte's nibbles swapped) isn't counted.
    w.apply(object(201, 3, world::ObjectType::Prim, [](world::Object& o) { o.state = 0xF1; }));

    AvatarComplexity c;
    AvatarComplexity::Limits limits;
    limits.maxComplexity = 5000;
    c.setLimits(limits);
    const Uuid someone = idOf(1), self = idOf(2), buddy = idOf(3);
    const auto isFriend = [&](const Uuid& id) { return id == buddy; };
    const auto now = std::chrono::steady_clock::now();
    c.update(w, noSources(), self, isFriend, now);

    const auto* info = c.find(someone);
    REQUIRE(info);
    CHECK(info->complexity > 20 * 500);
    CHECK(info->jelly);
    REQUIRE(c.find(self));
    CHECK(c.find(self)->complexity < 5000);
    CHECK_FALSE(c.find(self)->jelly);
    REQUIRE(c.find(buddy));
    const uint32_t buddyCost = c.find(buddy)->complexity;
    CHECK_FALSE(c.find(buddy)->jelly);
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{someone});
    const uint64_t revision = c.revision();

    // The HUD (point 31) isn't counted; the other one is.
    world::World noHud = w;
    noHud.apply(world::WorldUpdate{.region = kRegion, .change = world::WorldUpdate::Kill{{201}}});
    AvatarComplexity c2;
    c2.update(noHud, noSources(), self, isFriend, now);
    CHECK(c2.find(buddy)->complexity == buddyCost);

    // Picked to be shown whole; and the friend as a jelly doll.
    c.setChoice(someone, AvatarComplexity::Choice::Always);
    c.setChoice(buddy, AvatarComplexity::Choice::Never);
    c.decide(self, isFriend);
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{buddy});
    CHECK(c.revision() != revision);
    c.setChoice(someone, AvatarComplexity::Choice::Normal);
    c.setChoice(buddy, AvatarComplexity::Choice::Normal);

    // Only friends: everyone else (never us).
    limits.mode = AvatarComplexity::Mode::OnlyFriends;
    c.setLimits(limits);
    c.decide(self, isFriend);
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{someone});
    limits.maxComplexity = 0; // no limit doesn't matter to this mode
    c.setLimits(limits);
    c.decide(self, isFriend);
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{someone});

    // No limit: nobody.
    limits.mode = AvatarComplexity::Mode::Everyone;
    c.setLimits(limits);
    c.decide(self, isFriend);
    CHECK(c.jellyDolls().empty());

    // Friends let off the limit, however complex.
    limits.maxComplexity = 1;
    limits.mode = AvatarComplexity::Mode::FriendsAlways;
    c.setLimits(limits);
    c.decide(self, isFriend);
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{someone});

    // The surface area limit: what's worn adds up past it.
    limits.maxComplexity = 1000000;
    limits.mode = AvatarComplexity::Mode::Everyone;
    limits.maxArea = 5.0f; // 21 prims of 0.5 m: 10.5
    c.setLimits(limits);
    c.decide(self, isFriend);
    CHECK(c.find(someone)->area == doctest::Approx(10.5f));
    CHECK(c.jellyDolls() == std::unordered_set<Uuid>{someone});
}

TEST_CASE("Complexity: choices are kept in their file")
{
    const auto file = std::filesystem::temp_directory_path() /
                      std::format("obelisk-avatar-render-{}.llsd", std::rand());
    {
        AvatarComplexity c(file);
        c.setChoice(idOf(1), AvatarComplexity::Choice::Never);
        c.setChoice(idOf(2), AvatarComplexity::Choice::Always);
        c.setChoice(idOf(3), AvatarComplexity::Choice::Always);
        c.setChoice(idOf(3), AvatarComplexity::Choice::Normal);
    }
    AvatarComplexity again(file);
    CHECK(again.choice(idOf(1)) == AvatarComplexity::Choice::Never);
    CHECK(again.choice(idOf(2)) == AvatarComplexity::Choice::Always);
    CHECK(again.choice(idOf(3)) == AvatarComplexity::Choice::Normal);
    std::filesystem::remove(file);
}

TEST_CASE("Avatar render info: what's told and what's heard")
{
    const llsd::Value body = avatarRenderReportBody(
        {{idOf(1), 120000, true}, {idOf(2), 0, false}, {idOf(3), 4000000000u, false}});
    const llsd::Value& agents = body["agents"];
    REQUIRE(agents.isMap());
    CHECK(agents.size() == 2); // none with nothing added up
    CHECK(agents[idOf(1).toString()]["weight"].asInteger() == 120000);
    CHECK(agents[idOf(1).toString()]["tooComplex"].asBoolean());
    CHECK(agents[idOf(3).toString()]["weight"].asInteger() == INT32_MAX);

    llsd::Value answer;
    answer["agents"][idOf(5).toString()]["weight"] = llsd::Value(int32_t{80000});
    answer["agents"]["not an id"]["weight"] = llsd::Value(int32_t{1});
    answer["reportinglimit"] = llsd::Value(int32_t{6});
    answer["overlimit"] = llsd::Value(int32_t{2});
    const auto parsed = parseAvatarRenderAnswer(answer);
    REQUIRE(parsed);
    CHECK(parsed->weights.size() == 1);
    CHECK(parsed->weights.at(idOf(5)) == 80000);
    CHECK(parsed->reporting == 6);
    CHECK(parsed->overLimit == 2);
    CHECK_FALSE(parseAvatarRenderAnswer(llsd::Value(int32_t{3})));
}
