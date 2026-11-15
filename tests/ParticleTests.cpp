#include "viewer/world/ParticleSystem.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using viewer::world::decodeParticleSystem;
using viewer::world::ParticleSystem;

namespace
{

// Packs fields as LLDataPacker does (little-endian; fixed point).
struct Packer
{
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v)
    {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v)
    {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void fixed16(float v, int fracBits, float offset = 0.0f)
    {
        u16(static_cast<uint16_t>((v + offset) * static_cast<float>(1 << fracBits)));
    }
    void fixed8(float v, int fracBits)
    {
        u8(static_cast<uint8_t>(v * static_cast<float>(1 << fracBits)));
    }
    void uuid(uint8_t fill)
    {
        for(int i = 0; i < 16; i++)
        {
            u8(fill);
        }
    }
};

void system(Packer& p, uint32_t crc)
{
    p.u32(crc);
    p.u32(ParticleSystem::kNewAngles);
    p.u8(ParticleSystem::kAngleCone);
    p.fixed16(5.0f, 8);  // max age
    p.fixed16(0.5f, 8);  // start age
    p.fixed8(0.25f, 5);  // inner angle
    p.fixed8(1.5f, 5);   // outer angle
    p.fixed16(0.05f, 8); // burst rate: below the least, 0.01
    p.fixed16(2.0f, 8);  // radius
    p.fixed16(1.0f, 8);  // speed min
    p.fixed16(3.0f, 8);  // speed max
    p.u8(7);             // count
    p.fixed16(0.5f, 7, 256.0f);
    p.fixed16(0.0f, 7, 256.0f);
    p.fixed16(-1.0f, 7, 256.0f);
    p.fixed16(0.0f, 7, 256.0f);
    p.fixed16(0.0f, 7, 256.0f);
    p.fixed16(-9.75f, 7, 256.0f);
    p.uuid(0x11); // texture
    p.uuid(0x22); // target
}

void legacyParticle(Packer& p, uint32_t flags)
{
    p.u32(flags);
    p.fixed16(2.5f, 8); // max age
    for(const int c : {255, 0, 0, 255, 0, 0, 255, 0})
    {
        p.u8(static_cast<uint8_t>(c));
    }
    p.fixed8(0.5f, 5);
    p.fixed8(0.25f, 5);
    p.fixed8(1.0f, 5);
    p.fixed8(2.0f, 5);
}

} // namespace

TEST_CASE("Particles: the legacy block (86 bytes)")
{
    Packer p;
    system(p, 1234);
    legacyParticle(p, ParticleSystem::kInterpolateColor | ParticleSystem::kEmissive);
    REQUIRE(p.bytes.size() == 86);
    const auto s = decodeParticleSystem(p.bytes);
    REQUIRE(s);
    CHECK(s->crc == 1234);
    CHECK(s->flags == ParticleSystem::kNewAngles);
    CHECK(s->pattern == ParticleSystem::kAngleCone);
    CHECK(s->maxAge == doctest::Approx(5.0f));
    CHECK(s->startAge == doctest::Approx(0.5f));
    CHECK(s->innerAngle == doctest::Approx(0.25f));
    CHECK(s->outerAngle == doctest::Approx(1.5f));
    CHECK(s->burstRate == doctest::Approx(0.05f).epsilon(0.1));
    CHECK(s->burstRadius == doctest::Approx(2.0f));
    CHECK(s->burstSpeedMax == doctest::Approx(3.0f));
    CHECK(s->burstCount == 7);
    CHECK(s->angularVelocity.x == doctest::Approx(0.5f));
    CHECK(s->angularVelocity.z == doctest::Approx(-1.0f));
    CHECK(s->acceleration.z == doctest::Approx(-9.75f));
    CHECK(s->texture.bytes[0] == 0x11);
    CHECK(s->target.bytes[15] == 0x22);
    CHECK(s->particleFlags == (ParticleSystem::kInterpolateColor | ParticleSystem::kEmissive));
    CHECK(s->particleMaxAge == doctest::Approx(2.5f));
    CHECK(s->startColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(s->endColor == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    CHECK(s->startScale == glm::vec2(0.5f, 0.25f));
    CHECK(s->endScale == glm::vec2(1.0f, 2.0f));
    CHECK(s->startGlow == 0.0f);
    CHECK(s->blendSource == ParticleSystem::SourceAlpha);
    CHECK(s->blendDest == ParticleSystem::OneMinusSourceAlpha);
}

TEST_CASE("Particles: the sized block, with glow and blend functions")
{
    Packer p;
    p.u32(68);
    system(p, 99);
    p.u32(18 + 4);
    legacyParticle(p, ParticleSystem::kHasGlow | ParticleSystem::kHasBlend);
    p.u8(255); // start glow
    p.u8(51);  // end glow
    p.u8(ParticleSystem::SourceAlpha);
    p.u8(ParticleSystem::One); // additive
    const auto s = decodeParticleSystem(p.bytes);
    REQUIRE(s);
    CHECK(s->crc == 99);
    CHECK(s->startGlow == doctest::Approx(1.0f));
    CHECK(s->endGlow == doctest::Approx(0.2f));
    CHECK(s->blendSource == ParticleSystem::SourceAlpha);
    CHECK(s->blendDest == ParticleSystem::One);
}

TEST_CASE("Particles: no system, or one this doesn't know")
{
    CHECK_FALSE(decodeParticleSystem({}));
    Packer none;
    system(none, 0); // a checksum of 0: no system
    legacyParticle(none, 0);
    CHECK_FALSE(decodeParticleSystem(none.bytes));

    Packer bigger; // a system block of another size
    bigger.u32(72);
    system(bigger, 5);
    bigger.u32(0);
    bigger.u32(18);
    legacyParticle(bigger, 0);
    CHECK_FALSE(decodeParticleSystem(bigger.bytes));

    Packer extra; // particle parameters this doesn't know
    extra.u32(68);
    system(extra, 5);
    extra.u32(18 + 2);
    legacyParticle(extra, 0);
    extra.u16(0);
    CHECK_FALSE(decodeParticleSystem(extra.bytes));

    Packer badBlend; // a destination alpha factor: the default instead
    badBlend.u32(68);
    system(badBlend, 5);
    badBlend.u32(18 + 2);
    legacyParticle(badBlend, ParticleSystem::kHasBlend);
    badBlend.u8(6);
    badBlend.u8(ParticleSystem::One);
    const auto s = decodeParticleSystem(badBlend.bytes);
    REQUIRE(s);
    CHECK(s->blendSource == ParticleSystem::SourceAlpha);
    CHECK(s->blendDest == ParticleSystem::OneMinusSourceAlpha);
}

#include "ParticleView.hpp"

namespace
{

ParticleSystem dropper()
{
    ParticleSystem s;
    s.crc = 1;
    s.pattern = ParticleSystem::kDrop;
    s.burstRate = 0.1f;
    s.burstCount = 10;
    s.particleMaxAge = 1.0f;
    s.acceleration = glm::vec3(0.0f, 0.0f, -2.0f);
    return s;
}

// Runs `seconds` in steps of 1/50 s, the source at (10, 10, 20) each step
// (if `give`).
void run(viewer::ParticleView& view, const ParticleSystem& system, float seconds, bool give = true,
         uint32_t revision = 1)
{
    const viewer::world::ObjectKey key{1, 1};
    for(float t = 0.0f; t < seconds; t += 0.02f)
    {
        if(give)
        {
            view.source(key, system, revision, glm::vec3(10.0f, 10.0f, 20.0f),
                        glm::quat(1, 0, 0, 0));
        }
        view.update(0.02f, 0, glm::vec3(10.0f, 5.0f, 20.0f), 1000.0f, {}, {});
    }
}

} // namespace

TEST_CASE("Particles: a source emits bursts that fall and die at their max age")
{
    viewer::ParticleView view;
    run(view, dropper(), 0.5f);
    // About five bursts of ten, about half let through at first (the burst
    // rate throttle starts at a half).
    CHECK(view.count() > 5);
    CHECK(view.count() <= 60);
    CHECK(view.sources() == 1);

    // Gone (not given): no more are made, and those there die within a
    // second.
    run(view, dropper(), 1.1f, false);
    CHECK(view.sources() == 0);
    CHECK(view.count() == 0);
}

TEST_CASE("Particles: a source stops emitting at its max age, and starts over when changed")
{
    viewer::ParticleView view;
    ParticleSystem s = dropper();
    s.maxAge = 0.3f;
    run(view, s, 2.0f);
    CHECK(view.count() == 0); // its particles died a second after it stopped
    CHECK(view.sources() == 1);

    run(view, s, 0.2f); // the same, no update since: still nothing
    CHECK(view.count() == 0);
    run(view, s, 0.2f, true, 2); // sent again (the prim updated), unchanged
    CHECK(view.count() > 0);
}

TEST_CASE("Particles: too small to see, nothing is emitted")
{
    viewer::ParticleView view;
    ParticleSystem s = dropper();
    s.startScale = s.endScale = glm::vec2(0.0001f);
    run(view, s, 0.5f);
    CHECK(view.count() == 0);
}

TEST_CASE("Particles: drawn as asked; ribbons chained to the one after, the newest to the source")
{
    viewer::ParticleView view;
    ParticleSystem s = dropper();
    s.particleFlags = ParticleSystem::kRibbon | ParticleSystem::kEmissive;
    s.burstCount = 1;
    s.blendDest = ParticleSystem::One;
    run(view, s, 1.0f);
    REQUIRE(view.count() > 1);
    viewer::render::Scene scene;
    scene.camera.position = glm::vec3(10.0f, 5.0f, 20.0f);
    view.addTo(scene, [](const viewer::Uuid&, float) { return nullptr; }, 1000.0f);
    REQUIRE(scene.particles.size() == view.count());
    int toSource = 0;
    for(const auto& p : scene.particles)
    {
        CHECK(p.ribbon);
        CHECK(p.fullbright);
        CHECK(p.blendDest == ParticleSystem::One);
        CHECK(p.texture == nullptr); // the default image, still loading
        toSource += p.otherPosition == glm::vec3(10.0f, 10.0f, 20.0f) ? 1 : 0;
        CHECK(p.position.z <= 20.0f); // falling
    }
    CHECK(toSource == 1); // only the newest reaches back to the source
}
