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

#include <array>
#include <cstdint>
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

} // namespace viewer::volume
