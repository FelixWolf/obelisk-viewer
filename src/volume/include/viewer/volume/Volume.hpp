#pragma once

// Prim geometry: turns VolumeParams into triangle meshes, one per face.
//
// Behavior follows the official viewer's llvolume.cpp (LLProfile, LLPath,
// LLVolume::generate, LLVolumeFace::createSide/createCap/
// createUnCutCubeCap), so that shapes, vertex positions, texture
// coordinates and face numbering match what other viewers show. Face i of
// the result is texture entry i.
//
// Coordinates are in the unit prim (-0.5..0.5); scale by the object's size.

#include "viewer/volume/VolumeParams.hpp"

#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace viewer::volume
{

// Detail per level of detail (lowest to highest), as the official viewer.
inline constexpr std::array<float, 4> kDetailScales = {1.0f, 1.5f, 2.5f, 4.0f};

namespace FaceType
{
inline constexpr uint16_t Single = 0x0001;
inline constexpr uint16_t Cap = 0x0002;
inline constexpr uint16_t End = 0x0004;
inline constexpr uint16_t Side = 0x0008;
inline constexpr uint16_t Inner = 0x0010;
inline constexpr uint16_t Outer = 0x0020;
inline constexpr uint16_t Hollow = 0x0040;
inline constexpr uint16_t Open = 0x0080;
inline constexpr uint16_t Flat = 0x0100;
inline constexpr uint16_t Top = 0x0200;
inline constexpr uint16_t Bottom = 0x0400;
} // namespace FaceType

struct VolumeFace
{
    uint16_t type = 0; // FaceType bits
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals; // normalized
    std::vector<glm::vec2> texCoords;
    std::vector<uint32_t> indices; // counter-clockwise triangles
    // Rigged mesh only (else empty): per vertex, up to four joints (indices
    // into the mesh's skin joint list) and their weights, summing to 1.
    std::vector<glm::u8vec4> joints;
    std::vector<glm::vec4> weights;
};

// The faces of a prim at the given detail (one of kDetailScales). Returns no
// faces for parameters the official viewer can't build either.
std::vector<VolumeFace> generateVolume(const VolumeParams& params, float detail);

// A point of the path the profile is swept along: where, how big and how
// turned the profile is there, and its texture t.
struct PathPoint
{
    glm::vec3 position{0.0f};
    glm::vec2 scale{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float texT = 0.0f;
};

// The faces of a prim swept along `path` instead of its own path (flexible
// prims, bent by FlexibleView): an open line's faces, coordinates in
// whatever units the path's are.
std::vector<VolumeFace> generateVolumeAlong(const VolumeParams& params, float detail,
                                            std::span<const PathPoint> path);

// How a sculpt map's edges join: the sculpt type's low bits (llvolume.h
// LL_SCULPT_TYPE_*), plus flags.
namespace SculptStitching
{
inline constexpr uint8_t Sphere = 1;   // top and bottom rows pinched to points, sides wrap
inline constexpr uint8_t Torus = 2;    // sides wrap, top and bottom wrap
inline constexpr uint8_t Plane = 3;    // nothing wraps
inline constexpr uint8_t Cylinder = 4; // sides wrap
inline constexpr uint8_t Mask = 0x07;
inline constexpr uint8_t Invert = 0x40; // inside out
inline constexpr uint8_t Mirror = 0x80; // mirrored in x
} // namespace SculptStitching

// A sculpt map: RGBA8 pixels, rows bottom-up (as the official viewer keeps
// images, and TextureManager hands them out); R, G, B are x, y, z.
struct SculptMap
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> rgba;
};

// A sculpted prim's faces, as llvolume.cpp LLVolume::sculpt: the path and
// profile (circles, normally) get as many points as the map and the level of
// detail allow, and each mesh point comes from the map's pixel there instead
// of the sweep. A map too flat or too big to make a shape gives a sphere.
std::vector<VolumeFace> generateSculpt(const VolumeParams& params, float detail, uint8_t sculptType,
                                       const SculptMap& map);

} // namespace viewer::volume
