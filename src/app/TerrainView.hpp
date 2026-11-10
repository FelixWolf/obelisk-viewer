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

// Height lookup in global meters; returns nothing where no terrain is known.
using HeightSampler = std::function<std::optional<float>(int64_t globalX, int64_t globalY)>;

// One region as a 257x257 vertex grid (the last row and column come from
// the neighbours when known, so regions meet without gaps). Composition
// follows the region's start heights and ranges, roughened with noise.
TerrainMeshData buildTerrainMesh(const world::RegionTerrain& region, const HeightSampler& heightAt);

class TerrainView
{
public:
    // Rebuilds meshes for regions whose terrain changed.
    void update(const world::World& world, render::Renderer& renderer);

    // Adds every region's terrain, positioned relative to `originHandle`.
    using Material = std::variant<render::TerrainBlinnPhongMaterial, render::TerrainPbrMaterial>;
    void addTo(render::Scene& scene, uint64_t originHandle, const Material& material) const;

    void clear() { regions_.clear(); }

private:
    struct Region
    {
        render::Mesh mesh;
        uint32_t revision = 0;
        std::chrono::steady_clock::time_point builtAt{};
    };
    std::map<uint64_t, Region> regions_;
};

} // namespace viewer
