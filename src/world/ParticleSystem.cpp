#include "viewer/world/ParticleSystem.hpp"

#include <algorithm>
#include <cstring>

namespace viewer::world
{

namespace
{

constexpr size_t kSystemSize = 68;     // PS_SYS_DATA_BLOCK_SIZE
constexpr size_t kLegacyPartSize = 18; // PS_LEGACY_PART_DATA_BLOCK_SIZE
constexpr size_t kLegacySize = kSystemSize + kLegacyPartSize;
constexpr size_t kMaxSize = kLegacySize + 2 + 2 + 8; // glow, blend, two sizes

// Little-endian fields, as LLDataPackerBinaryBuffer packs them.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    bool failed() const { return failed_; }
    size_t remaining() const { return failed_ ? 0 : data_.size() - at_; }

    uint8_t u8()
    {
        if(failed_ || at_ >= data_.size())
        {
            failed_ = true;
            return 0;
        }
        return data_[at_++];
    }
    uint16_t u16()
    {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | static_cast<uint16_t>(u8()) << 8);
    }
    uint32_t u32()
    {
        const uint32_t lo = u16();
        return lo | static_cast<uint32_t>(u16()) << 16;
    }
    int32_t s32() { return static_cast<int32_t>(u32()); }
    Uuid uuid()
    {
        Uuid id;
        for(uint8_t& b : id.bytes)
        {
            b = u8();
        }
        return id;
    }
    // LLDataPacker::unpackFixed: integer and fraction bits (and a sign
    // bit, as an offset) in the fewest whole bytes.
    float fixed(bool isSigned, int intBits, int fracBits)
    {
        const int bits = intBits + fracBits + (isSigned ? 1 : 0);
        float value = 0.0f;
        if(bits <= 8)
        {
            value = u8();
        }
        else if(bits <= 16)
        {
            value = u16();
        }
        else
        {
            value = static_cast<float>(u32());
        }
        value /= static_cast<float>(1 << fracBits);
        if(isSigned)
        {
            value -= static_cast<float>(1 << intBits);
        }
        return value;
    }
    glm::vec4 color()
    {
        glm::vec4 c;
        for(int i = 0; i < 4; i++)
        {
            c[i] = static_cast<float>(u8()) / 255.0f;
        }
        return c;
    }

private:
    std::span<const uint8_t> data_;
    size_t at_ = 0;
    bool failed_ = false;
};

void readSystem(Reader& r, ParticleSystem& s)
{
    s.crc = r.u32();
    s.flags = r.u32();
    s.pattern = r.u8();
    s.maxAge = r.fixed(false, 8, 8);
    s.startAge = r.fixed(false, 8, 8);
    s.innerAngle = r.fixed(false, 3, 5);
    s.outerAngle = r.fixed(false, 3, 5);
    s.burstRate = std::max(0.01f, r.fixed(false, 8, 8));
    s.burstRadius = r.fixed(false, 8, 8);
    s.burstSpeedMin = r.fixed(false, 8, 8);
    s.burstSpeedMax = r.fixed(false, 8, 8);
    s.burstCount = r.u8();
    for(int i = 0; i < 3; i++)
    {
        s.angularVelocity[i] = r.fixed(true, 8, 7);
    }
    for(int i = 0; i < 3; i++)
    {
        s.acceleration[i] = r.fixed(true, 8, 7);
    }
    s.texture = r.uuid();
    s.target = r.uuid();
}

void readLegacyParticle(Reader& r, ParticleSystem& s)
{
    s.particleFlags = r.u32();
    s.particleMaxAge = r.fixed(false, 8, 8);
    s.startColor = r.color();
    s.endColor = r.color();
    s.startScale.x = r.fixed(false, 3, 5);
    s.startScale.y = r.fixed(false, 3, 5);
    s.endScale.x = r.fixed(false, 3, 5);
    s.endScale.y = r.fixed(false, 3, 5);
}

// Blend factors the renderer can use (not the destination alpha ones);
// others fall back to the default.
bool validBlend(uint8_t f)
{
    return f <= ParticleSystem::OneMinusSourceAlpha && f != 6 && f != 8;
}

} // namespace

std::optional<ParticleSystem> decodeParticleSystem(std::span<const uint8_t> data)
{
    if(data.empty() || data.size() > kMaxSize)
    {
        return std::nullopt;
    }
    Reader r(data);
    ParticleSystem s;
    if(data.size() == kLegacySize)
    {
        readSystem(r, s);
        readLegacyParticle(r, s);
    }
    else
    {
        // Sizes before each part; a system of another size is a version
        // this doesn't know.
        if(r.s32() != static_cast<int32_t>(kSystemSize))
        {
            return std::nullopt;
        }
        readSystem(r, s);
        int32_t size = r.s32() - static_cast<int32_t>(kLegacyPartSize);
        readLegacyParticle(r, s);
        if(s.particleFlags & ParticleSystem::kHasGlow)
        {
            if(size < 2)
            {
                return std::nullopt;
            }
            s.startGlow = static_cast<float>(r.u8()) / 255.0f;
            s.endGlow = static_cast<float>(r.u8()) / 255.0f;
            size -= 2;
        }
        if(s.particleFlags & ParticleSystem::kHasBlend)
        {
            if(size < 2)
            {
                return std::nullopt;
            }
            s.blendSource = r.u8();
            s.blendDest = r.u8();
            size -= 2;
        }
        if(size != 0)
        {
            return std::nullopt; // parameters this doesn't know: better nothing
        }
    }
    if(r.failed() || s.crc == 0)
    {
        return std::nullopt;
    }
    if(!validBlend(s.blendSource) || !validBlend(s.blendDest))
    {
        s.blendSource = ParticleSystem::SourceAlpha;
        s.blendDest = ParticleSystem::OneMinusSourceAlpha;
    }
    return s;
}

} // namespace viewer::world
