#pragma once

// Objects as the regions describe them: decoded from ObjectUpdate,
// ObjectUpdateCompressed and ImprovedTerseObjectUpdate. Formats follow the
// official viewer (llviewerobject.cpp processUpdateMessage, llvovolume.cpp,
// llprimitive.cpp parseTEMessage, llvolumemessage.cpp); our own code.

#include "viewer/core/Uuid.hpp"
#include "viewer/volume/VolumeParams.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace viewer::world
{

namespace ObjectType
{
inline constexpr uint8_t Prim = 9;
inline constexpr uint8_t Avatar = 47;
inline constexpr uint8_t Grass = 95;
inline constexpr uint8_t NewTree = 111;
inline constexpr uint8_t Tree = 255;
} // namespace ObjectType

// Texture entries come with up to 45 faces.
inline constexpr size_t kMaxFaces = 45;

struct TextureFace
{
    Uuid texture{};
    glm::vec4 color{1.0f};
    glm::vec2 repeats{1.0f};
    glm::vec2 offset{0.0f};
    float rotation = 0.0f; // radians
    uint8_t bumpShinyFullbright = 0;
    uint8_t mediaTexGen = 0;
    float glow = 0.0f;
    Uuid material{};

    bool fullbright() const { return (bumpShinyFullbright >> 5) & 1; }
    uint8_t shiny() const { return (bumpShinyFullbright >> 6) & 3; }
    uint8_t bump() const { return bumpShinyFullbright & 0x1F; }
    bool planarMapping() const { return (mediaTexGen & 0x06) == 0x02; }
};

using TextureEntry = std::array<TextureFace, kMaxFaces>;

// Decodes a packed texture entry (default value per field, then per-face
// overrides). Nothing for an empty or malformed one.
std::optional<TextureEntry> decodeTextureEntry(std::span<const uint8_t> data);

namespace SculptType
{
inline constexpr uint8_t None = 0;
inline constexpr uint8_t Sphere = 1;
inline constexpr uint8_t Torus = 2;
inline constexpr uint8_t Plane = 3;
inline constexpr uint8_t Cylinder = 4;
inline constexpr uint8_t Mesh = 5;
inline constexpr uint8_t Mask = 0x07;
} // namespace SculptType

// UpdateFlags bit: the object is temporary (object_flags.h); it's never cached.
inline constexpr uint32_t kFlagTemporaryOnRez = 1u << 29;

struct Object
{
    Uuid id{};
    uint32_t localId = 0;
    uint64_t region = 0;
    uint8_t type = 0; // ObjectType
    uint8_t state = 0;
    uint32_t parentId = 0; // local id of the parent, 0 for root objects
    uint32_t updateFlags = 0;

    // Relative to the parent if there is one, otherwise to the region.
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    glm::vec3 angularVelocity{0.0f};

    bool hasVolume = false;
    volume::VolumeParams volume{};
    TextureEntry faces{};
    uint8_t sculptType = SculptType::None; // with flags (invert 0x40, mirror 0x80)
    Uuid sculptTexture{};                  // or mesh asset
    bool flexible = false;
    bool animatedMesh = false; // animesh: its rigged mesh follows a skeleton of its own

    std::string hoverText{};
    // When the latest update (full or terse) arrived, and the simulator's
    // time dilation then: motion is predicted from here (extrapolate()).
    // Default (epoch): never updated over the network; nothing to predict.
    std::chrono::steady_clock::time_point updatedAt{};
    float timeDilation = 1.0f;
    uint32_t revision = 0;      // bumped on every change
    uint32_t shapeRevision = 0; // bumped when the geometry changes
};

// The pieces of one update, applied to an Object by applyUpdate().
struct ObjectUpdateData
{
    enum class Kind : uint8_t
    {
        Full,  // replaces everything
        Terse, // motion only (and optionally a texture entry)
    };
    Kind kind = Kind::Full;
    uint32_t localId = 0;
    Object full{};
    // Compressed updates: the object's checksum, and (if it may be cached:
    // not temporary) the block as received, for the object cache.
    uint32_t crc = 0;
    std::vector<uint8_t> compressed{};
    // Terse:
    uint8_t state = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    glm::vec3 angularVelocity{0.0f};
    std::optional<TextureEntry> faces{};
};

// One object of a compressed update (the ObjectData.Data bytes).
std::optional<ObjectUpdateData> decodeCompressedObject(std::span<const uint8_t> data,
                                                       uint32_t updateFlags);
// One object of a terse update.
std::optional<ObjectUpdateData> decodeTerseObject(std::span<const uint8_t> data,
                                                  std::span<const uint8_t> textureEntry);

// Extra parameters (sculpt, flexible, ...) from their packed form.
void applyExtraParams(Object& object, std::span<const uint8_t> data);

// Applies an update; returns whether the geometry changed.
bool applyUpdate(Object& object, const ObjectUpdateData& update);

struct Pose
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Where an object is `seconds` of simulator time after its latest update,
// from that update's velocity, acceleration and angular velocity (dead
// reckoning, as llviewerobject.cpp's interpolateLinearMotion and
// applyAngularVelocity). Only root objects move in a line; anything spins.
// The caller limits `seconds` (see ObjectMotion in the app).
Pose extrapolate(const Object& object, float seconds);

// The same from values: `linear` false for children (they only spin).
struct Motion
{
    Pose pose{};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    glm::vec3 angularVelocity{0.0f};
};
Pose extrapolate(const Motion& motion, bool linear, float seconds);

} // namespace viewer::world
