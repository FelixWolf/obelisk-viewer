#include "viewer/world/Objects.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace viewer::world
{

namespace
{

// Little-endian reader with bounds checks; reading past the end sets
// failed() and returns zeros.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    bool failed() const { return failed_; }
    size_t remaining() const { return failed_ ? 0 : data_.size() - pos_; }

    bool bytes(void* out, size_t size)
    {
        if(failed_ || data_.size() - pos_ < size)
        {
            failed_ = true;
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    void skip(size_t size)
    {
        if(failed_ || data_.size() - pos_ < size)
        {
            failed_ = true;
            return;
        }
        pos_ += size;
    }
    uint8_t u8()
    {
        uint8_t v = 0;
        bytes(&v, 1);
        return v;
    }
    uint16_t u16()
    {
        uint8_t b[2] = {};
        bytes(b, 2);
        return static_cast<uint16_t>(b[0] | b[1] << 8);
    }
    uint32_t u32()
    {
        uint8_t b[4] = {};
        bytes(b, 4);
        return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
               static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    glm::vec3 vec3()
    {
        const float x = f32();
        const float y = f32();
        const float z = f32();
        return {x, y, z};
    }
    Uuid uuid()
    {
        Uuid id;
        bytes(id.bytes.data(), 16);
        return id;
    }
    std::string string()
    {
        std::string s;
        while(!failed_)
        {
            const char c = static_cast<char>(u8());
            if(c == 0)
            {
                break;
            }
            s.push_back(c);
        }
        return s;
    }
    // A 4-byte length, then that many bytes.
    std::span<const uint8_t> binary()
    {
        const uint32_t size = u32();
        if(failed_ || data_.size() - pos_ < size)
        {
            failed_ = true;
            return {};
        }
        const auto out = data_.subspan(pos_, size);
        pos_ += size;
        return out;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
    bool failed_ = false;
};

// A rotation sent as x, y, z of a unit quaternion (w >= 0 rebuilt).
glm::quat quatFromVector(const glm::vec3& v)
{
    const float w2 = 1.0f - glm::dot(v, v);
    return glm::quat(w2 > 0.0f ? std::sqrt(w2) : 0.0f, v.x, v.y, v.z);
}

// U16 -> float in [lower, upper], snapping values within one step of zero
// to zero (as the official viewer's U16_to_F32).
float unquantize(uint16_t value, float lower, float upper)
{
    const float delta = upper - lower;
    float v = static_cast<float>(value) / 65535.0f * delta + lower;
    if(std::abs(v) < delta / 65535.0f)
    {
        v = 0.0f;
    }
    return v;
}

volume::PackedVolumeParams readPackedVolume(Reader& r)
{
    volume::PackedVolumeParams p;
    p.pathCurve = r.u8();
    p.pathBegin = r.u16();
    p.pathEnd = r.u16();
    p.pathScaleX = r.u8();
    p.pathScaleY = r.u8();
    p.pathShearX = static_cast<int8_t>(r.u8());
    p.pathShearY = static_cast<int8_t>(r.u8());
    p.pathTwist = static_cast<int8_t>(r.u8());
    p.pathTwistBegin = static_cast<int8_t>(r.u8());
    p.pathRadiusOffset = static_cast<int8_t>(r.u8());
    p.pathTaperX = static_cast<int8_t>(r.u8());
    p.pathTaperY = static_cast<int8_t>(r.u8());
    p.pathRevolutions = r.u8();
    p.pathSkew = static_cast<int8_t>(r.u8());
    p.profileCurve = r.u8();
    p.profileBegin = r.u16();
    p.profileEnd = r.u16();
    p.profileHollow = r.u16();
    return p;
}

// One texture entry field: a default for every face, then runs of
// (face bitfield, value) until a zero bitfield.
template <typename T, typename Read>
bool readField(Reader& r, TextureEntry& faces, T TextureFace::* member, Read read)
{
    const T first = read(r);
    if(r.failed())
    {
        return false;
    }
    for(TextureFace& f : faces)
    {
        f.*member = first;
    }
    while(r.remaining() > 0)
    {
        uint64_t mask = 0;
        uint8_t b = 0;
        do
        {
            b = r.u8();
            mask = (mask << 7) | (b & 0x7F);
        } while((b & 0x80) && !r.failed());
        if(r.failed())
        {
            return false;
        }
        if(mask == 0)
        {
            break;
        }
        const T value = read(r);
        if(r.failed())
        {
            return false;
        }
        for(size_t i = 0; i < kMaxFaces; i++)
        {
            if(mask & (1ull << i))
            {
                faces[i].*member = value;
            }
        }
    }
    return true;
}

} // namespace

std::optional<TextureEntry> decodeTextureEntry(std::span<const uint8_t> data)
{
    if(data.empty())
    {
        return std::nullopt;
    }
    // The last field isn't zero-terminated; add the terminator.
    std::vector<uint8_t> buffer(data.begin(), data.end());
    buffer.push_back(0);
    Reader r(buffer);
    TextureEntry faces{};

    const bool ok =
        readField(r, faces, &TextureFace::texture, [](Reader& x) { return x.uuid(); }) &&
        readField(r, faces, &TextureFace::color,
                  [](Reader& x)
                  {
                      // Sent inverted, so white is all zeros.
                      glm::vec4 c;
                      for(int i = 0; i < 4; i++)
                      {
                          c[i] = static_cast<float>(255 - x.u8()) / 255.0f;
                      }
                      return c;
                  }) &&
        readField(r, faces, &TextureFace::repeats,
                  [](Reader& x) { return glm::vec2(x.f32(), 0.0f); }) &&
        [&]
    {
        // repeats.y is its own field; read it into a scratch entry.
        TextureEntry t = faces;
        if(!readField(r, t, &TextureFace::repeats,
                      [](Reader& x) { return glm::vec2(x.f32(), 0.0f); }))
        {
            return false;
        }
        for(size_t i = 0; i < kMaxFaces; i++)
        {
            faces[i].repeats.y = t[i].repeats.x;
        }
        return true;
    }() &&
        [&]
    {
        TextureEntry s = faces;
        TextureEntry t = faces;
        auto offset = [](Reader& x)
        { return glm::vec2(static_cast<float>(static_cast<int16_t>(x.u16())) / 32767.0f, 0.0f); };
        if(!readField(r, s, &TextureFace::offset, offset) ||
           !readField(r, t, &TextureFace::offset, offset))
        {
            return false;
        }
        for(size_t i = 0; i < kMaxFaces; i++)
        {
            faces[i].offset = glm::vec2(s[i].offset.x, t[i].offset.x);
        }
        return true;
    }() &&
        readField(r, faces, &TextureFace::rotation,
                  [](Reader& x)
                  {
                      return static_cast<float>(static_cast<int16_t>(x.u16())) / 32768.0f *
                             6.283185307f;
                  }) &&
        readField(r, faces, &TextureFace::bumpShinyFullbright, [](Reader& x) { return x.u8(); }) &&
        readField(r, faces, &TextureFace::mediaTexGen, [](Reader& x) { return x.u8(); }) &&
        readField(r, faces, &TextureFace::glow,
                  [](Reader& x) { return static_cast<float>(x.u8()) / 255.0f; });
    if(!ok)
    {
        return std::nullopt;
    }
    // Material ids are optional (newer simulators append them).
    if(r.remaining() > 0)
    {
        readField(r, faces, &TextureFace::material, [](Reader& x) { return x.uuid(); });
    }
    return faces;
}

void applyExtraParams(Object& object, std::span<const uint8_t> data)
{
    object.sculptType = SculptType::None;
    object.sculptTexture = {};
    object.flexible = false;
    object.animatedMesh = false;
    object.light.reset();
    object.lightImage.reset();
    object.materials.clear();
    if(data.empty())
    {
        return;
    }
    Reader r(data);
    const uint8_t count = r.u8();
    for(uint8_t i = 0; i < count && !r.failed(); i++)
    {
        const uint16_t type = r.u16();
        const auto block = r.binary();
        Reader p(block);
        switch(type)
        {
        case 0x10: // flexible
            object.flexible = true;
            break;
        case 0x30: // sculpt: texture (or mesh asset) and type
        case 0x60: // mesh: same layout (asset id, type 5)
            object.sculptTexture = p.uuid();
            object.sculptType = p.u8();
            break;
        case 0x70: // extended mesh: flags (llprimitive.h ANIMATED_MESH_ENABLED_FLAG)
            object.animatedMesh = (p.u32() & 0x1) != 0;
            break;
        case 0x20: // light: color (alpha: intensity), radius, cutoff, falloff
        {
            LightParams light;
            const uint8_t r8 = p.u8();
            const uint8_t g8 = p.u8();
            const uint8_t b8 = p.u8();
            const uint8_t a8 = p.u8();
            light.color = glm::vec3(r8, g8, b8) / 255.0f;
            light.intensity = static_cast<float>(a8) / 255.0f;
            light.radius = p.f32();
            light.cutoff = p.f32();
            light.falloff = p.f32();
            if(!p.failed())
            {
                object.light = light;
            }
            break;
        }
        case 0x80: // render materials: count, then (face, material asset id)
        {
            const uint8_t entries = p.u8();
            for(uint8_t e = 0; e < entries && !p.failed(); e++)
            {
                const uint8_t face = p.u8();
                const Uuid id = p.uuid();
                if(!p.failed() && face < kMaxFaces && !id.isNull())
                {
                    object.materials.emplace_back(face, id);
                }
            }
            break;
        }
        case 0x40: // light image: texture, then fov, focus, ambiance
        {
            LightImage image;
            image.texture = p.uuid();
            image.fov = p.f32();
            image.focus = p.f32();
            image.ambiance = p.f32();
            if(!p.failed())
            {
                object.lightImage = image;
            }
            break;
        }
        default:
            break;
        }
    }
}

std::optional<ObjectUpdateData> decodeCompressedObject(std::span<const uint8_t> data,
                                                       uint32_t updateFlags)
{
    Reader r(data);
    ObjectUpdateData update;
    update.kind = ObjectUpdateData::Kind::Full;
    Object& o = update.full;
    o.id = r.uuid();
    o.localId = r.u32();
    update.localId = o.localId;
    o.type = r.u8();
    o.state = r.u8();
    o.updateFlags = updateFlags;
    update.crc = r.u32();
    r.u8(); // material (physical)
    r.u8(); // click action
    o.scale = r.vec3();
    o.position = r.vec3();
    o.rotation = quatFromVector(r.vec3());
    const uint32_t flags = r.u32();
    r.uuid(); // owner
    if(flags & 0x80)
    {
        o.angularVelocity = r.vec3();
    }
    o.parentId = (flags & 0x20) ? r.u32() : 0;
    if(flags & 0x02)
    {
        o.species = r.u8(); // trees and grass
    }
    else if(flags & 0x01)
    {
        r.u32(); // scratch pad size, then the scratch pad itself
        r.binary();
    }
    if(flags & 0x04)
    {
        o.hoverText = r.string();
        r.skip(4); // text color
    }
    if(flags & 0x200)
    {
        r.string(); // media URL
    }
    if(flags & 0x08)
    {
        r.skip(86); // particle system (legacy block: 68 system + 18 particle bytes)
    }
    {
        // Extra parameters, re-packed so applyExtraParams can read them.
        std::vector<uint8_t> extra;
        const uint8_t count = r.u8();
        extra.push_back(count);
        for(uint8_t i = 0; i < count && !r.failed(); i++)
        {
            const uint16_t type = r.u16();
            const auto block = r.binary();
            extra.push_back(static_cast<uint8_t>(type));
            extra.push_back(static_cast<uint8_t>(type >> 8));
            const auto size = static_cast<uint32_t>(block.size());
            for(int b = 0; b < 4; b++)
            {
                extra.push_back(static_cast<uint8_t>(size >> (8 * b)));
            }
            extra.insert(extra.end(), block.begin(), block.end());
        }
        applyExtraParams(o, extra);
    }
    if(flags & 0x10)
    {
        r.uuid(); // sound
        r.f32();  // gain
        r.u8();   // flags
        r.f32();  // radius
    }
    if(flags & 0x100)
    {
        r.string(); // name/value pairs
    }
    if(o.type == ObjectType::Prim)
    {
        o.volume = volume::unpackVolumeParams(readPackedVolume(r));
        o.hasVolume = true;
        if(auto faces = decodeTextureEntry(r.binary()))
        {
            o.faces = *faces;
        }
        // Texture animation (flags & 0x40) follows; not used yet.
    }
    if(r.failed())
    {
        return std::nullopt;
    }
    return update;
}

std::optional<ObjectUpdateData> decodeTerseObject(std::span<const uint8_t> data,
                                                  std::span<const uint8_t> textureEntry)
{
    Reader r(data);
    ObjectUpdateData update;
    update.kind = ObjectUpdateData::Kind::Terse;
    update.localId = r.u32();
    update.state = r.u8();
    if(r.u8() != 0)
    {
        r.skip(16); // avatar foot collision plane
    }
    update.position = r.vec3();
    glm::vec3 v;
    for(int i = 0; i < 3; i++)
    {
        v[i] = unquantize(r.u16(), -128.0f, 128.0f);
    }
    update.velocity = v;
    for(int i = 0; i < 3; i++)
    {
        v[i] = unquantize(r.u16(), -64.0f, 64.0f);
    }
    update.acceleration = v;
    glm::vec4 q;
    for(int i = 0; i < 4; i++)
    {
        q[i] = unquantize(r.u16(), -1.0f, 1.0f);
    }
    update.rotation = glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
    for(int i = 0; i < 3; i++)
    {
        v[i] = unquantize(r.u16(), -64.0f, 64.0f);
    }
    update.angularVelocity = v;
    if(r.failed())
    {
        return std::nullopt;
    }
    // Here the texture entry is length-prefixed (it goes through the binary
    // packer, as in llvovolume.cpp's terse path).
    if(textureEntry.size() > 4)
    {
        Reader te(textureEntry);
        update.faces = decodeTextureEntry(te.binary());
    }
    return update;
}

bool applyUpdate(Object& object, const ObjectUpdateData& update)
{
    object.revision++;
    if(update.kind == ObjectUpdateData::Kind::Terse)
    {
        object.state = update.state;
        object.position = update.position;
        object.rotation = update.rotation;
        object.velocity = update.velocity;
        object.acceleration = update.acceleration;
        object.angularVelocity = update.angularVelocity;
        if(update.faces)
        {
            object.faces = *update.faces;
        }
        return false;
    }
    const Object& next = update.full;
    const bool shapeChanged =
        !object.hasVolume || object.volume != next.volume || object.sculptType != next.sculptType ||
        object.sculptTexture != next.sculptTexture || object.type != next.type;
    const uint32_t revision = object.revision;
    const uint32_t shapeRevision = object.shapeRevision;
    object = next;
    object.revision = revision;
    object.shapeRevision = shapeRevision + (shapeChanged ? 1 : 0);
    return shapeChanged;
}

Pose extrapolate(const Motion& motion, bool linear, float seconds)
{
    Pose pose = motion.pose;
    if(seconds <= 0.0f)
    {
        return pose;
    }
    if(linear)
    {
        // Velocities are the average over the simulator's last physics step,
        // hence the step subtracted from the acceleration term.
        constexpr float kPhysicsStep = 1.0f / 45.0f;
        pose.position += motion.velocity * seconds +
                         0.5f * motion.acceleration * seconds * (seconds - kPhysicsStep);
    }
    const float omega = glm::length(motion.angularVelocity);
    if(omega > 0.003f) // the official viewer's 0.00001 on the squared length
    {
        // About an axis in the parent's (or region's) frame.
        pose.rotation = glm::normalize(
            glm::angleAxis(omega * seconds, motion.angularVelocity / omega) * pose.rotation);
    }
    return pose;
}

Pose extrapolate(const Object& object, float seconds)
{
    return extrapolate(Motion{{object.position, object.rotation},
                              object.velocity,
                              object.acceleration,
                              object.angularVelocity},
                       object.parentId == 0, seconds);
}

} // namespace viewer::world
