#pragma once

// Automatic reflection probes, placed as the official viewer places them
// (LLReflectionMapManager::registerSpatialGroup, LLReflectionMap::
// autoAdjustOrigin): its octree of objects gets a probe in every node 32 m
// across, and such nodes exist where the 64 m node around them held more
// than 128 objects (OctreeMaxNodeCapacity), so only dense places get them.
// Here a grid stands in for the octree: 64 m cells counting the objects that
// fit them, 32 m cells inside the full ones.
//
// A probe sits at the middle of its objects' bounds, at least 2 m above the
// ground, and far enough up that half its radius (its near clip) clears the
// ground too; its radius reaches the bounds' corners, 8 m at least.

#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace viewer
{

struct ProbeObject
{
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// `groundAt` gives the ground's height at a scene position. Probe ids come
// from their cells, mixed with `idSeed` (so cells of different scene
// origins don't share ids).
std::vector<render::ReflectionProbe>
placeAutomaticProbes(std::span<const ProbeObject> objects,
                     const std::function<float(glm::vec2)>& groundAt, uint64_t idSeed);

} // namespace viewer
