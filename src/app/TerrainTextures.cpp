#include "TerrainTextures.hpp"

#include "viewer/image/TextureProcessing.hpp"

#include <algorithm>
#include <cmath>

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

image::TextureData solid(uint8_t r, uint8_t g, uint8_t b)
{
    image::TextureData t;
    t.levels.push_back(image::MipLevel{1, 1, {r, g, b, 255}});
    return t;
}

} // namespace

const render::GpuTexture* TerrainTextures::array(const Key& key, TextureManager& textures)
{
    wanted_.insert(key);
    for(const Uuid& id : key.ids)
    {
        textures.data(id); // keeps them loading (and the copies kept)
    }
    const auto it = arrays_.find(key);
    return it != arrays_.end() ? &it->second.texture : nullptr;
}

void TerrainTextures::layerTransform(const assets::TextureTransform& t, glm::vec3& row0,
                                     glm::vec3& row1)
{
    // (x, -y), scaled, turned by [[c, s], [-s, c]] (textureUtilV.glsl's
    // khr_texture_transform), offset, then v flipped back.
    const glm::vec2 scale = t.scale / 8.0f;
    const float c = std::cos(t.rotation);
    const float s = std::sin(t.rotation);
    row0 = glm::vec3(c * scale.x, -s * scale.y, t.offset.x);
    row1 = glm::vec3(s * scale.x, c * scale.y, -t.offset.y);
}

TerrainView::Material TerrainTextures::material(const world::RegionTerrain& region,
                                                TextureManager& textures,
                                                MaterialManager& materials,
                                                const TerrainOverrides::Layers* overrides,
                                                const render::TerrainBlinnPhongMaterial& fallback)
{
    std::array<Uuid, 4> layers = kDefaultLayers;
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

    // Texture terrain if any of them is a texture.
    bool anyTexture = false;
    for(const Uuid& id : layers)
    {
        const image::TextureData* data = textures.data(id);
        anyTexture = anyTexture || (data && !data->levels.empty());
    }
    if(anyTexture)
    {
        const render::GpuTexture* diffuse = array({layers, Kind::Diffuse}, textures);
        if(!diffuse)
        {
            return fallback;
        }
        render::TerrainBlinnPhongMaterial m;
        m.diffuseLayers = diffuse;
        return m;
    }

    // Otherwise PBR, once all four materials are in (with the region's
    // overrides).
    std::array<assets::Material, 4> found;
    for(size_t i = 0; i < 4; i++)
    {
        const auto loaded = materials.get(layers[i]);
        if(!loaded)
        {
            return fallback;
        }
        found[i] = overrides && (*overrides)[i] ? assets::applyOverride(*loaded, *(*overrides)[i])
                                                : *loaded;
    }
    auto layerKey = [&](assets::MaterialTexture slot, Kind kind)
    {
        Key key{{}, kind};
        for(size_t i = 0; i < 4; i++)
        {
            key.ids[i] = found[i].textures[slot];
        }
        return key;
    };
    render::TerrainPbrMaterial m;
    m.albedoLayers = array(layerKey(assets::kBaseColorTexture, Kind::Albedo), textures);
    m.ormLayers = array(layerKey(assets::kMetallicRoughnessTexture, Kind::Orm), textures);
    m.emissiveLayers = array(layerKey(assets::kEmissiveTexture, Kind::Emissive), textures);
    m.normalLayers = array(layerKey(assets::kNormalTexture, Kind::Normal), textures);
    if(!m.albedoLayers || !m.ormLayers || !m.emissiveLayers || !m.normalLayers)
    {
        return fallback;
    }
    for(size_t i = 0; i < 4; i++)
    {
        const assets::Material& source = found[i];
        render::TerrainPbrMaterial::Layer& layer = m.layers[i];
        layer.baseColorFactor = source.baseColor;
        layer.emissiveTint = source.emissive;
        layer.emissiveStrength = 1.0f;
        layer.roughness = source.roughness;
        layer.metallic = source.metallic;
        // R occlusion, applied to the image-based light (pbrterrainF.glsl at
        // the default RenderTerrainPBRDetail 0).
        layer.occlusionStrength = 1.0f;
        layerTransform(source.transforms[assets::kBaseColorTexture], layer.uvRow0, layer.uvRow1);
    }
    return m;
}

void TerrainTextures::update(TextureManager& textures, render::Renderer& renderer)
{
    for(const Key& key : wanted_)
    {
        // Each layer's whole image, or what stands in for none.
        std::array<image::TextureData, 4> standIns;
        std::array<const image::TextureData*, 4> data{};
        bool ready = true;
        for(size_t i = 0; i < 4; i++)
        {
            if(key.ids[i].isNull())
            {
                switch(key.kind)
                {
                case Kind::Emissive:
                    standIns[i] = solid(0, 0, 0);
                    break;
                case Kind::Normal:
                    standIns[i] = solid(128, 128, 255);
                    break;
                default:
                    standIns[i] = solid(255, 255, 255);
                    break;
                }
                data[i] = &standIns[i];
                continue;
            }
            data[i] = textures.data(key.ids[i]);
            ready = ready && data[i] && !data[i]->levels.empty();
        }
        if(!ready)
        {
            continue;
        }
        // A common size: the largest layer's, capped. Built again only if
        // that has grown (a bigger copy arrived).
        uint32_t size = 1;
        for(const auto* d : data)
        {
            size = std::max({size, d->levels.front().width, d->levels.front().height});
        }
        size = std::min(size, kMaxLayerSize);
        const auto it = arrays_.find(key);
        if(it != arrays_.end() && it->second.size >= size)
        {
            continue;
        }
        std::array<image::TextureData, 4> layers;
        std::array<const image::TextureData*, 4> pointers{};
        for(size_t i = 0; i < 4; i++)
        {
            layers[i].levels =
                image::buildMipChain(image::resizeRgba8(data[i]->levels.front(), size, size));
            pointers[i] = &layers[i];
        }
        const bool srgb =
            key.kind == Kind::Diffuse || key.kind == Kind::Albedo || key.kind == Kind::Emissive;
        if(render::GpuTexture built = renderer.uploadTextureArray(pointers, srgb))
        {
            arrays_[key] = Array{std::move(built), size};
        }
    }
    wanted_.clear();
}

} // namespace viewer
