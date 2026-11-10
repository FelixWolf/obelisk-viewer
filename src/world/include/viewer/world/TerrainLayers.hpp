#pragma once

// Which of the four terrain detail layers shows where, computed the way the
// official viewer does (behavior checked against llvlcomposition.cpp
// generateHeights, llsurfacepatch.cpp eval and noise.h/noise.cpp; our own
// implementation).
//
// The layer value is the terrain height plus 2D Perlin noise, measured
// against the region's start height and height range for the four layers.
// The noise tables are built from a fixed seed. The official viewer feeds that
// seed to the C library's rand(), so its noise (and terrain blending)
// differs between platforms; we always use the Microsoft C runtime's
// generator, i.e. what the Windows viewer shows.

#include "viewer/world/World.hpp"

#include <cstdint>
#include <vector>

namespace viewer::world
{

// Perlin noise with the official viewer's lattice and tables.
float terrainNoise2(float x, float y);
// Two octaves of terrainNoise2, as the viewer's turbulence2(v, 2).
float terrainTurbulence2(float x, float y);

// The layer value (0..3) at sample (x, y) of a region (x, y in 0..255).
float terrainLayerValue(const RegionTerrain& region, uint32_t x, uint32_t y);

// All 256x256 layer values of a region, row-major.
std::vector<float> terrainLayerValues(const RegionTerrain& region);

// A second, low-frequency noise (0..1) that varies how sharp the transitions
// between layers are (the terrain blend ramp's second coordinate).
float terrainBlendNoise(double globalX, double globalY);

} // namespace viewer::world
