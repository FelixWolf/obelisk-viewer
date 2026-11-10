#pragma once

// The four detail textures of each region's terrain, as the 4-layer array
// the terrain shaders sample. Regions name their textures in RegionHandshake;
// unset ones use the official viewer's defaults (dirt, grass, mountain, rock).
// A region draws with the placeholder material until all four are loaded.

#include "TextureManager.hpp"

#include "viewer/render/Material.hpp"
#include "viewer/world/World.hpp"

#include <array>
#include <map>
#include <set>

namespace viewer
{

class TerrainTextures
{
public:
    // The material for a region: its own textures once loaded, otherwise
    // `fallback`. Starts loading them on first use.
    render::TerrainBlinnPhongMaterial material(const world::RegionTerrain& region,
                                               TextureManager& textures,
                                               const render::TerrainBlinnPhongMaterial& fallback);

    // Builds the arrays whose four layers have all loaded.
    void update(TextureManager& textures, render::Renderer& renderer);

    size_t readyCount() const { return arrays_.size(); }

private:
    using Layers = std::array<Uuid, 4>;
    static Layers layersFor(const world::RegionTerrain& region);

    std::set<Layers> wanted_;
    std::map<Layers, render::GpuTexture> arrays_;
};

} // namespace viewer
