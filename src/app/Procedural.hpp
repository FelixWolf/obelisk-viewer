#pragma once

// Procedural textures and meshes: stand-ins until textures and objects come
// from the grid, and a fixed scene to check the shaders against.

#include "viewer/image/TextureData.hpp"
#include "viewer/render/Mesh.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace viewer::procedural
{

struct MeshData
{
    std::vector<render::ObjectVertex> vertices;
    std::vector<uint32_t> indices;
};

// Unit-radius UV sphere around the origin, with MikkTSpace-style tangents.
MeshData sphere(uint32_t segments = 128, uint32_t rings = 64);
// Cube from -1 to 1, one UV square per face.
MeshData cube();

// Smooth 2D value noise in [0, 1], tiling with the given period (in cells).
float noise(float x, float y, int period);
// Fractal noise in [0, 1].
float fbm(float x, float y, int octaves, int period);

// 256x256 RGBA8 textures with full mip chains.
image::TextureData checkerDiffuse();
image::TextureData bumpNormal();     // A = glossiness
image::TextureData stripeSpecular(); // RGB specular color
image::TextureData tileAlbedo();
image::TextureData tileOrm();
image::TextureData tileEmissive();
image::TextureData tileNormal();

// Four terrain layers, low to high: dirt, grass, rock, snow.
std::array<image::TextureData, 4> terrainDiffuse();
std::array<image::TextureData, 4> terrainOrm();
std::array<image::TextureData, 4> terrainNormal();

} // namespace viewer::procedural
