#pragma once

// Each region's terrain materials, as the arrays the terrain shaders
// sample. Regions name four detail assets in RegionHandshake (unset ones
// use the official viewer's default textures: dirt, grass, mountain,
// rock); as the official viewer (LLTerrainMaterials::getMaterialType) each
// is asked for both as a texture and as a PBR material: texture terrain if
// any of them loads as a texture, otherwise PBR terrain once all four
// materials have. A region draws with the placeholder until its layers
// are in.
//
// Layer arrays are built from the textures' whole images (CPU copies, at
// most kMaxLayerSize across) and built again whenever a bigger copy of
// one arrives, so terrain isn't left at whatever size loaded first.

#include "MaterialManager.hpp"
#include "TerrainOverrides.hpp"
#include "TerrainView.hpp"
#include "TextureManager.hpp"

#include "viewer/render/Material.hpp"
#include "viewer/world/World.hpp"

#include <array>
#include <compare>
#include <map>
#include <set>

namespace viewer
{

class TerrainTextures
{
public:
    // The material for a region: its own layers once loaded, otherwise
    // `fallback`. Starts loading them on first use; call every frame.
    // PBR layers get the region's material overrides (if any) and each
    // layer's texture transform (`layerTransform`).
    TerrainView::Material material(const world::RegionTerrain& region, TextureManager& textures,
                                   MaterialManager& materials,
                                   const TerrainOverrides::Layers* overrides,
                                   const render::TerrainBlinnPhongMaterial& fallback);

    // A PBR terrain layer's texture coordinates from region-local meters
    // (lldrawpoolterrain.cpp, terrain_texture_transform): its material's
    // base color transform (KHR_texture_transform, with v flipped around
    // it; one transform for every map), its scale divided by
    // RenderTerrainPBRScale (8 m).
    static void layerTransform(const assets::TextureTransform& t, glm::vec3& row0, glm::vec3& row1);

    // Builds (or rebuilds bigger) the arrays asked for since the last
    // update whose layers have all loaded.
    void update(TextureManager& textures, render::Renderer& renderer);

    size_t readyCount() const { return arrays_.size(); }

    // Layers share one size in an array; larger textures are scaled down.
    static constexpr uint32_t kMaxLayerSize = 1024;

private:
    // What an array holds, and what stands in for a layer without one.
    enum class Kind : uint8_t
    {
        Diffuse,  // texture terrain (sRGB)
        Albedo,   // PBR base color (sRGB; none: white)
        Orm,      // PBR metallic-roughness (linear; none: white)
        Emissive, // PBR (sRGB; none: black)
        Normal,   // PBR (linear; none: flat)
    };
    struct Key
    {
        std::array<Uuid, 4> ids{};
        Kind kind = Kind::Diffuse;
        auto operator<=>(const Key&) const = default;
    };
    struct Array
    {
        render::GpuTexture texture;
        uint32_t size = 0; // layer size it was built at
    };
    // The array, if built (asks for its textures either way).
    const render::GpuTexture* array(const Key& key, TextureManager& textures);

    std::set<Key> wanted_;
    std::map<Key, Array> arrays_;
};

} // namespace viewer
