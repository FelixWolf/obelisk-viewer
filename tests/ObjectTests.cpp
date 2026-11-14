#include "viewer/world/Objects.hpp"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <bit>

using namespace viewer::world;

namespace
{

void put16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void putFloat(std::vector<uint8_t>& out, float f)
{
    const auto v = std::bit_cast<uint32_t>(f);
    for(int i = 0; i < 4; i++)
    {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void putUuid(std::vector<uint8_t>& out, uint8_t fill)
{
    out.insert(out.end(), 16, fill);
}

} // namespace

TEST_CASE("Texture entries: defaults plus per-face overrides")
{
    std::vector<uint8_t> te;
    // Textures: default all 0x11; faces 1 and 3 (bits 0b1010) get 0x22.
    putUuid(te, 0x11);
    te.push_back(0x0A);
    putUuid(te, 0x22);
    te.push_back(0);
    // Colors (inverted): default white; face 2 (bit 0b100) half-transparent red.
    te.insert(te.end(), {0, 0, 0, 0});
    te.push_back(0x04);
    te.insert(te.end(), {0, 255, 255, 128});
    te.push_back(0);
    // Repeats U and V: 1 and 2.
    putFloat(te, 1.0f);
    te.push_back(0);
    putFloat(te, 2.0f);
    te.push_back(0);
    // Offsets U and V, rotation.
    put16(te, 0);
    te.push_back(0);
    put16(te, static_cast<uint16_t>(-16384)); // -0.5
    te.push_back(0);
    put16(te, 8192); // a quarter turn
    te.push_back(0);
    // Bump/shiny/fullbright: face 0 fullbright, via a two-byte bitfield
    // (0x80 continues: the mask is (0 << 7) | 1).
    te.push_back(0);
    te.insert(te.end(), {0x80, 0x01});
    te.push_back(0x20);
    te.push_back(0);
    // Media, glow (last field: no terminator, as sent).
    te.push_back(0);
    te.push_back(0);
    te.push_back(51);

    const auto faces = decodeTextureEntry(te);
    REQUIRE(faces.has_value());
    CHECK((*faces)[0].texture.bytes[0] == 0x11);
    CHECK((*faces)[1].texture.bytes[0] == 0x22);
    CHECK((*faces)[2].texture.bytes[0] == 0x11);
    CHECK((*faces)[3].texture.bytes[0] == 0x22);
    CHECK((*faces)[44].texture.bytes[0] == 0x11);
    CHECK((*faces)[0].color == glm::vec4(1.0f));
    CHECK((*faces)[2].color.r == doctest::Approx(1.0f));
    CHECK((*faces)[2].color.g == doctest::Approx(0.0f));
    CHECK((*faces)[2].color.a == doctest::Approx(127.0f / 255.0f));
    CHECK((*faces)[5].repeats == glm::vec2(1.0f, 2.0f));
    CHECK((*faces)[5].offset.y == doctest::Approx(-0.5f).epsilon(0.001));
    CHECK((*faces)[5].rotation == doctest::Approx(3.14159265f / 2.0f).epsilon(0.001));
    CHECK((*faces)[0].fullbright());
    CHECK_FALSE((*faces)[1].fullbright());
    CHECK((*faces)[7].glow == doctest::Approx(0.2f));

    CHECK_FALSE(decodeTextureEntry({}).has_value());
    te.resize(10); // truncated
    CHECK_FALSE(decodeTextureEntry(te).has_value());
}

TEST_CASE("Terse updates decode motion")
{
    std::vector<uint8_t> data;
    data.insert(data.end(), {0x39, 0x30, 0, 0}); // local id 12345
    data.push_back(0);                           // state
    data.push_back(0);                           // not an avatar
    putFloat(data, 10.0f);
    putFloat(data, 20.0f);
    putFloat(data, 30.0f);
    for(int i = 0; i < 6; i++)
    {
        put16(data, 32768); // velocity, acceleration ~ 0
    }
    put16(data, 32768); // rotation x, y, z ~ 0, w = 1
    put16(data, 32768);
    put16(data, 32768);
    put16(data, 65535);
    for(int i = 0; i < 3; i++)
    {
        put16(data, 32768);
    }
    const auto update = decodeTerseObject(data, {});
    REQUIRE(update.has_value());
    CHECK(update->localId == 12345);
    CHECK(update->position == glm::vec3(10.0f, 20.0f, 30.0f));
    CHECK(update->rotation.w == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(glm::length(update->velocity) < 0.01f);

    // A texture entry in a terse update carries a 4-byte length first.
    // Every field's default and no overrides: texture, color, repeats u/v,
    // offsets u/v, rotation, bump, media, glow (last: no terminator).
    std::vector<uint8_t> fields(16, 0x5A);
    fields.push_back(0);
    fields.insert(fields.end(), {0, 0, 0, 0, 0});
    for(int i = 0; i < 2; i++)
    {
        fields.insert(fields.end(), {0, 0, 0x80, 0x3F, 0}); // 1.0f
    }
    for(int i = 0; i < 3; i++)
    {
        fields.insert(fields.end(), {0, 0, 0});
    }
    fields.insert(fields.end(), {0, 0, 0, 0, 0});
    std::vector<uint8_t> te = {static_cast<uint8_t>(fields.size()), 0, 0, 0};
    te.insert(te.end(), fields.begin(), fields.end());
    const auto withTexture = decodeTerseObject(data, te);
    REQUIRE(withTexture.has_value());
    REQUIRE(withTexture->faces.has_value());
    CHECK(withTexture->faces->at(3).texture.bytes[0] == 0x5A);

    data.resize(20);
    CHECK_FALSE(decodeTerseObject(data, {}).has_value());
}

TEST_CASE("Extra parameters: sculpts and meshes")
{
    // Captured from a mesh object on Aditi: one parameter, type 0x0060
    // (mesh), 17 bytes: asset id, sculpt type 5.
    const std::vector<uint8_t> mesh = {0x01, 0x60, 0x00, 0x11, 0x00, 0x00, 0x00, 0x67,
                                       0x36, 0x6a, 0x75, 0x41, 0xfa, 0x93, 0x6e, 0x51,
                                       0x63, 0xfa, 0xa7, 0xa2, 0x5c, 0x83, 0x59, 0x05};
    Object o;
    applyExtraParams(o, mesh);
    CHECK((o.sculptType & SculptType::Mask) == SculptType::Mesh);
    CHECK(o.sculptTexture.toString() == "67366a75-41fa-936e-5163-faa7a25c8359");

    // A sculpt (type 0x30) with sphere stitching, plus a flexible block.
    std::vector<uint8_t> sculpt = {0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x30, 0x00, 0x11, 0x00, 0x00, 0x00};
    sculpt.insert(sculpt.end(), 16, 0xAB);
    sculpt.push_back(0x01);
    applyExtraParams(o, sculpt);
    CHECK(o.flexible);
    CHECK(o.flexible->softness == 2); // too short: the defaults
    CHECK(o.sculptType == SculptType::Sphere);

    // A flexible block: softness 3 (top bits of tension and drag), tension
    // 1.5, drag 0.5, gravity 0.5, wind 1, force (0, 0, 2).
    std::vector<uint8_t> flexi = {0x01, 0x10,      0x00,     0x10, 0x00, 0x00,
                                  0x00, 0x80 | 15, 0x80 | 5, 105,  10};
    for(const float f : {0.0f, 0.0f, 2.0f})
    {
        const auto bits = std::bit_cast<std::array<uint8_t, 4>>(f);
        flexi.insert(flexi.end(), bits.begin(), bits.end());
    }
    applyExtraParams(o, flexi);
    REQUIRE(o.flexible);
    CHECK(o.flexible->softness == 3);
    CHECK(o.flexible->tension == doctest::Approx(1.5f));
    CHECK(o.flexible->drag == doctest::Approx(0.5f));
    CHECK(o.flexible->gravity == doctest::Approx(0.5f));
    CHECK(o.flexible->wind == doctest::Approx(1.0f));
    CHECK(o.flexible->force.z == 2.0f);
    applyExtraParams(o, sculpt);
    CHECK(o.sculptTexture.bytes[0] == 0xAB);
    CHECK_FALSE(o.animatedMesh);

    // Extended mesh (type 0x70): flags, bit 0 makes it an animated object.
    const std::vector<uint8_t> animated = {0x01, 0x70, 0x00, 0x04, 0x00, 0x00,
                                           0x00, 0x01, 0x00, 0x00, 0x00};
    applyExtraParams(o, animated);
    CHECK(o.animatedMesh);
    applyExtraParams(o, {});
    CHECK_FALSE(o.animatedMesh);
}

TEST_CASE("Motion between updates is predicted from velocity, acceleration and spin")
{
    using viewer::world::extrapolate;
    viewer::world::Object o;
    o.position = glm::vec3(10.0f, 20.0f, 30.0f);
    o.velocity = glm::vec3(2.0f, 0.0f, 0.0f);

    CHECK(extrapolate(o, 0.0f).position == o.position);
    CHECK(extrapolate(o, -1.0f).position == o.position);
    const glm::vec3 moved = extrapolate(o, 0.5f).position;
    CHECK(moved.x == doctest::Approx(11.0f));
    CHECK(moved.y == doctest::Approx(20.0f));

    // Falling: half a t (t - physics step) of acceleration.
    o.velocity = glm::vec3(0.0f);
    o.acceleration = glm::vec3(0.0f, 0.0f, -9.8f);
    const float t = 1.0f;
    CHECK(extrapolate(o, t).position.z ==
          doctest::Approx(30.0f - 0.5f * 9.8f * t * (t - 1.0f / 45.0f)));

    // Spinning a quarter turn about +Z in a second.
    o.acceleration = glm::vec3(0.0f);
    o.angularVelocity = glm::vec3(0.0f, 0.0f, glm::half_pi<float>());
    const glm::vec3 east = extrapolate(o, 1.0f).rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(east.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(east.y == doctest::Approx(1.0f));

    // Children only spin; their parent carries them.
    o.parentId = 5;
    o.velocity = glm::vec3(2.0f, 0.0f, 0.0f);
    CHECK(extrapolate(o, 1.0f).position == o.position);
}

TEST_CASE("Extra parameters: lights and projectors")
{
    auto f32 = [](std::vector<uint8_t>& out, float v)
    {
        const auto bits = std::bit_cast<uint32_t>(v);
        for(int i = 0; i < 4; i++)
        {
            out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    };
    // Two parameters: a light (0x20, 16 bytes) and a light image (0x40, 28).
    std::vector<uint8_t> data = {0x02, 0x20, 0x00, 0x10, 0x00, 0x00, 0x00, 255, 128, 0, 51};
    f32(data, 6.0f);  // radius
    f32(data, 0.0f);  // cutoff
    f32(data, 0.75f); // falloff
    data.insert(data.end(), {0x40, 0x00, 0x1C, 0x00, 0x00, 0x00});
    data.insert(data.end(), 16, 0xCD);
    f32(data, 1.0f); // fov
    f32(data, 2.0f); // focus
    f32(data, 0.5f); // ambiance

    Object o;
    applyExtraParams(o, data);
    REQUIRE(o.light.has_value());
    CHECK(o.light->color.r == doctest::Approx(1.0f));
    CHECK(o.light->color.g == doctest::Approx(128.0f / 255.0f));
    CHECK(o.light->intensity == doctest::Approx(0.2f));
    CHECK(o.light->radius == 6.0f);
    CHECK(o.light->falloff == 0.75f);
    REQUIRE(o.lightImage.has_value());
    CHECK(o.lightImage->texture.bytes[0] == 0xCD);
    CHECK(o.lightImage->fov == 1.0f);
    CHECK(o.lightImage->ambiance == 0.5f);

    applyExtraParams(o, {});
    CHECK_FALSE(o.light.has_value());
    CHECK_FALSE(o.lightImage.has_value());
}

TEST_CASE("Extra parameters: reflection probes")
{
    auto f32 = [](std::vector<uint8_t>& out, float v)
    {
        const auto bits = std::bit_cast<uint32_t>(v);
        for(int i = 0; i < 4; i++)
        {
            out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    };
    // One parameter: a reflection probe (0x90, 9 bytes), a box.
    std::vector<uint8_t> data = {0x01, 0x90, 0x00, 0x09, 0x00, 0x00, 0x00};
    f32(data, 2.5f); // ambiance
    f32(data, 1.5f); // clip distance
    data.push_back(0x01);

    Object o;
    applyExtraParams(o, data);
    REQUIRE(o.reflectionProbe.has_value());
    CHECK(o.reflectionProbe->ambiance == 2.5f);
    CHECK(o.reflectionProbe->clipDistance == 1.5f);
    CHECK(o.reflectionProbe->box());

    applyExtraParams(o, {});
    CHECK_FALSE(o.reflectionProbe.has_value());
}

TEST_CASE("Name values: names and values, values with spaces")
{
    const auto pairs = viewer::world::parseNameValues(
        "FirstName STRING RW SV Automated\nLastName STRING RW SV Tests\r\n"
        "Title STRING RW SV Ralph pls go\nbroken line\n");
    REQUIRE(pairs.size() == 3);
    CHECK(pairs[0].first == "FirstName");
    CHECK(pairs[0].second == "Automated");
    CHECK(pairs[1].second == "Tests");
    CHECK(pairs[2].first == "Title");
    CHECK(pairs[2].second == "Ralph pls go");
    CHECK(viewer::world::parseNameValues("").empty());
}

namespace
{

std::vector<uint8_t> packTextureAnimation(uint8_t mode, int8_t face, uint8_t x, uint8_t y,
                                          float start, float length, float rate)
{
    std::vector<uint8_t> data{mode, static_cast<uint8_t>(face), x, y};
    for(const float f : {start, length, rate})
    {
        const auto bits = std::bit_cast<std::array<uint8_t, 4>>(f);
        data.insert(data.end(), bits.begin(), bits.end());
    }
    return data;
}

} // namespace

TEST_CASE("Texture animation: decoding")
{
    using viewer::world::TextureAnimation;
    const auto a = viewer::world::decodeTextureAnimation(
        packTextureAnimation(TextureAnimation::On | TextureAnimation::Loop, 2, 4, 2, 1.0f, 0.0f,
                             8.0f),
        true);
    REQUIRE(a);
    CHECK(a->face == 2);
    CHECK(a->sizeX == 4);
    CHECK(a->sizeY == 2);
    CHECK(a->start == 1.0f);
    CHECK(a->rate == 8.0f);
    // Full updates count an empty grid as one frame, unless smooth.
    CHECK(viewer::world::decodeTextureAnimation(
              packTextureAnimation(TextureAnimation::On, -1, 0, 0, 0, 0, 1), true)
              ->sizeX == 1);
    CHECK(viewer::world::decodeTextureAnimation(
              packTextureAnimation(TextureAnimation::On, -1, 0, 0, 0, 0, 1), false)
              ->sizeX == 0);
    CHECK_FALSE(viewer::world::decodeTextureAnimation(std::vector<uint8_t>(15), true));
}

TEST_CASE("Texture animation: flipbooks, smooth scrolling, rotation")
{
    using viewer::world::animateTexture;
    using viewer::world::TextureAnimation;
    TextureAnimation flip;
    flip.mode = TextureAnimation::On | TextureAnimation::Loop;
    flip.sizeX = 4;
    flip.sizeY = 2;
    // Frame 0: the top left cell, a quarter by a half of the texture.
    auto t = animateTexture(flip, 0.0f);
    REQUIRE(t);
    CHECK(t->translates);
    CHECK(t->scales);
    CHECK(t->scaleS == doctest::Approx(0.25f));
    CHECK(t->scaleT == doctest::Approx(0.5f));
    CHECK(t->offsetS == doctest::Approx(-0.375f));
    CHECK(t->offsetT == doctest::Approx(0.25f));
    // Frame 5: second row, second column; whole frames only.
    t = animateTexture(flip, 5.4f);
    CHECK(t->offsetS == doctest::Approx(-0.125f));
    CHECK(t->offsetT == doctest::Approx(-0.25f));
    // Looping: frame 9 of 8 is frame 1.
    CHECK(animateTexture(flip, 9.0f)->offsetS == doctest::Approx(-0.125f));
    // Not looping: it stays on the last.
    flip.mode = TextureAnimation::On;
    CHECK(animateTexture(flip, 100.0f)->offsetT == doctest::Approx(-0.25f));
    CHECK(animateTexture(flip, 100.0f)->offsetS == doctest::Approx(0.375f));
    // Ping-pong: 0 1 2 ... 7 6 5 ...; frame 9 is 5.
    flip.mode = TextureAnimation::On | TextureAnimation::Loop | TextureAnimation::PingPong;
    CHECK(animateTexture(flip, 9.0f)->offsetS == doctest::Approx(-0.125f));
    CHECK(animateTexture(flip, 9.0f)->offsetT == doctest::Approx(-0.25f));

    // Smooth scrolling: no grid, offset by the frame.
    TextureAnimation scroll;
    scroll.mode = TextureAnimation::On | TextureAnimation::Loop | TextureAnimation::Smooth;
    scroll.sizeX = scroll.sizeY = 0;
    scroll.length = 1.0f;
    t = animateTexture(scroll, 2.25f);
    REQUIRE(t);
    CHECK(t->translates);
    CHECK_FALSE(t->scales);
    CHECK(t->offsetS == doctest::Approx(0.25f));

    // Rotation: radians, nothing else.
    TextureAnimation spin;
    spin.mode = TextureAnimation::On | TextureAnimation::Loop | TextureAnimation::Smooth |
                TextureAnimation::Rotate;
    spin.length = 6.283f;
    t = animateTexture(spin, 1.5f);
    CHECK(t->rotates);
    CHECK_FALSE(t->translates);
    CHECK(t->rotation == doctest::Approx(1.5f));

    // Off: nothing.
    spin.mode = 0;
    CHECK_FALSE(animateTexture(spin, 1.0f));
}
