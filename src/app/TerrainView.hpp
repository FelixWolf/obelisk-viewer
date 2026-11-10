#pragma once

// Turns World terrain into GPU meshes and adds them to the scene. Meshes are
// rebuilt when a region's terrain changes (throttled while patches stream in).

#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace viewer
{

struct TerrainMeshData
{
    std::vector<render::TerrainVertex> vertices;
    std::vector<uint32_t> indices;
};

// Finds a region's terrain by handle; nullptr if unknown.
using RegionLookup = std::function<const world::RegionTerrain*(uint64_t handle)>;

// One region as a 257x257 vertex grid. The last row and column come from
// the east/north neighbours when known, so regions meet without gaps. Layer
// values and blend noise follow the official viewer (see TerrainLayers.hpp),
// including how it takes layer values from the neighbour at the far edges.
TerrainMeshData buildTerrainMesh(const world::RegionTerrain& region, const RegionLookup& lookup);

class TerrainView
{
public:
    // Rebuilds meshes for regions whose terrain changed.
    void update(const world::World& world, render::Renderer& renderer);

    // Adds every region's terrain, positioned relative to `originHandle`,
    // with the material `materialFor` picks for each region.
    using Material = std::variant<render::TerrainBlinnPhongMaterial, render::TerrainPbrMaterial>;
    using MaterialFor = std::function<Material(uint64_t regionHandle)>;
    void addTo(render::Scene& scene, uint64_t originHandle, const MaterialFor& materialFor) const;

    void clear() { regions_.clear(); }

private:
    struct Region
    {
        render::Mesh mesh;
        uint64_t revision = 0; // of this region and the neighbours its edges use
        std::chrono::steady_clock::time_point builtAt{};
    };
    std::map<uint64_t, Region> regions_;
};

} // namespace viewer
