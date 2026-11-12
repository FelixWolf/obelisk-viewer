#include "viewer/world/Objects.hpp"

#include <doctest/doctest.h>

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
    CHECK(o.sculptType == SculptType::Sphere);
    CHECK(o.sculptTexture.bytes[0] == 0xAB);
    CHECK_FALSE(o.animatedMesh);

    // Extended mesh (type 0x70): flags, bit 0 makes it an animated object.
    const std::vector<uint8_t> animated = {0x01, 0x70, 0x00, 0x04, 0x00, 0x00, 0x00,
                                           0x01, 0x00, 0x00, 0x00};
    applyExtraParams(o, animated);
    CHECK(o.animatedMesh);
    applyExtraParams(o, {});
    CHECK_FALSE(o.animatedMesh);
}
