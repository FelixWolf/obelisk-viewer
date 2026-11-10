#include "TerrainTextures.hpp"

#include "viewer/image/TextureProcessing.hpp"

#include <algorithm>

namespace viewer
{

namespace
{

// The official viewer's defaults for unset terrain textures (asset ids from
// indra_constants.cpp).
const std::array<Uuid, 4> kDefaultLayers = {
    *Uuid::parse("0bc58228-74a0-7e83-89bc-5c23464bcec5"), // dirt
    *Uuid::parse("63338ede-0037-c4fd-855b-015d77112fc8"), // grass
    *Uuid::parse("303cd381-8560-7579-23f1-f0a880799740"), // mountain
    *Uuid::parse("53a2f406-4895-1d13-d541-d2e3b86bc19c"), // rock
};

// Layers share one size in the array; larger textures are scaled down.
constexpr uint32_t kMaxLayerSize = 1024;

} // namespace

TerrainTextures::Layers TerrainTextures::layersFor(const world::RegionTerrain& region)
{
    Layers layers = kDefaultLayers;
    if(region.hasComposition)
    {
        for(size_t i = 0; i < 4; i++)
        {
            if(!region.composition.detailTextures[i].isNull())
            {
                layers[i] = region.composition.detailTextures[i];
            }
        }
    }
    return layers;
}

render::TerrainBlinnPhongMaterial
TerrainTextures::material(const world::RegionTerrain& region, TextureManager& textures,
                          const render::TerrainBlinnPhongMaterial& fallback)
{
    const Layers layers = layersFor(region);
    if(const auto it = arrays_.find(layers); it != arrays_.end())
    {
        render::TerrainBlinnPhongMaterial m;
        m.diffuseLayers = &it->second;
        return m;
    }
    if(wanted_.insert(layers).second)
    {
        for(const Uuid& id : layers)
        {
            textures.data(id); // start loading, keeping a CPU copy
        }
    }
    return fallback;
}

void TerrainTextures::update(TextureManager& textures, render::Renderer& renderer)
{
    for(auto it = wanted_.begin(); it != wanted_.end();)
    {
        std::array<const image::TextureData*, 4> data{};
        bool ready = true;
        for(size_t i = 0; i < 4; i++)
        {
            data[i] = textures.data((*it)[i]);
            ready = ready && data[i] && !data[i]->levels.empty();
        }
        if(!ready)
        {
            ++it;
            continue;
        }

        // A common size: the largest layer's, capped.
        uint32_t size = 1;
        for(const auto* d : data)
        {
            size = std::max({size, d->width(), d->height()});
        }
        size = std::min(size, kMaxLayerSize);
        std::array<image::TextureData, 4> layers;
        std::array<const image::TextureData*, 4> pointers{};
        for(size_t i = 0; i < 4; i++)
        {
            layers[i].levels =
                image::buildMipChain(image::resizeRgba8(data[i]->levels.front(), size, size));
            pointers[i] = &layers[i];
        }
        if(render::GpuTexture array = renderer.uploadTextureArray(pointers, true))
        {
            arrays_.emplace(*it, std::move(array));
        }
        it = wanted_.erase(it);
    }
}

} // namespace viewer
