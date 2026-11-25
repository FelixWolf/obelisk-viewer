#pragma once

// Objects as the regions describe them: decoded from ObjectUpdate,
// ObjectUpdateCompressed and ImprovedTerseObjectUpdate. Formats follow the
// official viewer (llviewerobject.cpp processUpdateMessage, llvovolume.cpp,
// llprimitive.cpp parseTEMessage, llvolumemessage.cpp); our own code.

#include "viewer/core/Uuid.hpp"
#include "viewer/volume/VolumeParams.hpp"
#include "viewer/world/ParticleSystem.hpp"
#include "viewer/world/TextureAnimation.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    // Shows media (MOAP: the object's ObjectMedia has this face's).
    bool hasMedia() const { return (mediaTexGen & 0x01) != 0; }
};

using TextureEntry = std::array<TextureFace, kMaxFaces>;

// Name-value pairs (an object update's NameValue): one a line, "Name Type
// Class Sendto Value", such as "FirstName STRING RW SV Kyler"; avatars have
// FirstName, LastName and Title (their group title). Name and value only.
std::vector<std::pair<std::string, std::string>> parseNameValues(std::string_view text);

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

// A flexible prim (extra parameter 0x10, llprimitive.cpp
// LLFlexibleObjectData): how it bends (FlexibleView simulates it). The
// defaults are the official viewer's.
struct FlexibleParams
{
    int softness = 2;      // 0..3: the path is simulated in 2^softness sections
    float tension = 1.0f;  // 0..10
    float drag = 2.0f;     // air friction, 0..10
    float gravity = 0.3f;  // -10..10
    float wind = 0.0f;     // wind sensitivity, 0..10
    glm::vec3 force{0.0f}; // user force, region axes
    bool operator==(const FlexibleParams&) const = default;
};

// A prim that gives light (extra parameter 0x20, llprimitive.cpp LLLightParams).
struct LightParams
{
    glm::vec3 color{1.0f}; // linear
    float intensity = 1.0f;
    float radius = 10.0f; // meters
    float cutoff = 0.0f;
    float falloff = 0.75f;
};

// A light that projects (0x40, LLLightImageParams): a spotlight along the
// prim's -Z, with an optional texture.
struct LightImage
{
    Uuid texture{};
    float fov = 0.0f; // radians
    float focus = 0.0f;
    float ambiance = 0.0f;
};

// A reflection probe (0x90, LLReflectionProbeParams): the prim's volume
// (a sphere its X size across, or its box) takes its reflections and
// ambient light from a cube captured at its center.
struct ReflectionProbeParams
{
    float ambiance = 0.0f;     // how much ambient light comes from it (at least the sky's)
    float clipDistance = 0.0f; // the capture's near clip, meters
    uint8_t flags = 0;
    static constexpr uint8_t kBox = 0x01;     // a box, not a sphere
    static constexpr uint8_t kDynamic = 0x02; // captures avatars too
    static constexpr uint8_t kMirror = 0x04;  // a mirror's (not drawn yet)
    bool box() const { return (flags & kBox) != 0; }
};

// UpdateFlags bits (object_flags.h).
inline constexpr uint32_t kFlagUsePhysics = 1u << 0;
inline constexpr uint32_t kFlagHandleTouch = 1u << 7; // a script listens for touches
inline constexpr uint32_t kFlagTakesMoney = 1u << 9;
// The object is temporary; it's never cached.
inline constexpr uint32_t kFlagTemporaryOnRez = 1u << 29;

// What a left click does (llSetClickAction; indra_constants.h).
namespace ClickAction
{
inline constexpr uint8_t Touch = 0;
inline constexpr uint8_t Sit = 1;
inline constexpr uint8_t Buy = 2;
inline constexpr uint8_t Pay = 3;
inline constexpr uint8_t Open = 4;
inline constexpr uint8_t Play = 5;
inline constexpr uint8_t OpenMedia = 6;
inline constexpr uint8_t Zoom = 7;
inline constexpr uint8_t Disabled = 8; // not even a touch
inline constexpr uint8_t Ignore = 9;   // clicks go through it
} // namespace ClickAction

// Sound flags (llaudioengine.h LL_SOUND_FLAG_*).
namespace SoundFlags
{
inline constexpr uint8_t Loop = 0x01;
inline constexpr uint8_t SyncMaster = 0x02;
inline constexpr uint8_t SyncSlave = 0x04;
inline constexpr uint8_t SyncPending = 0x08;
inline constexpr uint8_t Queue = 0x10;
inline constexpr uint8_t Stop = 0x20;
} // namespace SoundFlags

struct ObjectSound
{
    Uuid id{}; // null: none
    Uuid owner{};
    float gain = 0.0f;
    uint8_t flags = 0;   // SoundFlags
    float radius = 0.0f; // heard only within this of the listener (under 0.1: anywhere)
    bool operator==(const ObjectSound&) const = default;
};

struct Object
{
    Uuid id{};
    uint32_t localId = 0;
    uint64_t region = 0;
    uint8_t type = 0; // ObjectType
    uint8_t state = 0;
    uint8_t species = 0;   // trees and grass: which kind
    uint32_t parentId = 0; // local id of the parent, 0 for root objects
    uint32_t updateFlags = 0;
    uint8_t clickAction = ClickAction::Touch;
    // Its physical material (llmaterialtable.h LL_MCODE_*: 0 stone, 1 metal,
    // 2 glass, 3 wood, 4 flesh, 5 plastic, 6 rubber), for footsteps.
    uint8_t material = 0;

    // Relative to the parent if there is one, otherwise to the region.
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    glm::vec3 angularVelocity{0.0f};
    // Avatars: the plane they stand on (normal and offset, region
    // coordinates), as the simulator says; zero: none.
    glm::vec4 footPlane{0.0f};

    bool hasVolume = false;
    volume::VolumeParams volume{};
    TextureEntry faces{};
    uint8_t sculptType = SculptType::None; // with flags (invert 0x40, mirror 0x80)
    Uuid sculptTexture{};                  // or mesh asset
    std::optional<FlexibleParams> flexible;
    bool animatedMesh = false; // animesh: its rigged mesh follows a skeleton of its own
    std::optional<LightParams> light;
    std::optional<LightImage> lightImage;
    std::optional<ReflectionProbeParams> reflectionProbe;
    // PBR material asset per face (extra parameter 0x80; null: none). Their
    // overrides come separately (World::materialOverrides).
    std::vector<std::pair<uint8_t, Uuid>> materials;

    // Floating text (llSetText) and its color, RGBA (alpha as sent flipped
    // back: 255 is opaque).
    std::string hoverText{};
    std::array<uint8_t, 4> hoverTextColor{255, 255, 255, 255};
    // Its media's version ("x-mv:0000000003/<agent>", llSetPrimMediaParams
    // and the like bump it; empty: it never had media): when it changes,
    // its faces' media are asked for again (ObjectMedia capability).
    std::string mediaVersion{};
    // Texture animation, if any, and how far it had run (frames) when it
    // (re)started at `textureAnimationSince`: World keeps these across
    // updates (an update with one restarts it, a smooth one carries on).
    std::optional<TextureAnimation> textureAnimation{};
    std::chrono::steady_clock::time_point textureAnimationSince{};
    float textureAnimationFrames = 0.0f;
    // Its particle system (llParticleSystem), if it has one.
    std::optional<ParticleSystem> particles{};
    // The sound it plays (llPlaySound, llLoopSound, ...), from its full and
    // compressed updates and AttachedSound. Each of those sets it again
    // (`soundEvents` counts them), as the official viewer calls
    // setAttachedSound for each: a null sound stops a loop, the same sound
    // not looping plays again. AttachedSoundGainChange only changes the gain.
    ObjectSound sound{};
    uint32_t soundEvents = 0;
    // Avatars: their legacy name and group title (from NameValue).
    std::string firstName{};
    std::string lastName{};
    std::string groupTitle{};
    // An attachment: the inventory item it was attached from (NameValue
    // AttachItemID).
    Uuid attachItemId{};
    // When the latest update (full or terse) arrived, and the simulator's
    // time dilation then: motion is predicted from here (extrapolate()).
    // Default (epoch): never updated over the network; nothing to predict.
    std::chrono::steady_clock::time_point updatedAt{};
    float timeDilation = 1.0f;
    uint32_t revision = 0;      // bumped on every change
    uint32_t shapeRevision = 0; // bumped when the geometry changes
    // Bumped when what it is or looks like changes (not when it only moves).
    uint32_t contentRevision = 0;
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
    // A compressed update that says the object's particle system is as it
    // was (flag 0x400 without the legacy block): keep it.
    bool keepParticles = false;
    // From the object cache: a sound it had then plays only if it loops (a
    // one-off would play again each time it's loaded).
    bool fromCache = false;
    // Terse:
    uint8_t state = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    glm::vec3 angularVelocity{0.0f};
    std::optional<glm::vec4> footPlane{};
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
