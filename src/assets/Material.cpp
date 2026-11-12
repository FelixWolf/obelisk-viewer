#include "viewer/assets/Material.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <algorithm>

namespace viewer::assets
{

namespace
{

std::optional<glm::vec2> vec2(const llsd::Value& v)
{
    if(!v.isArray() || v.size() < 2)
    {
        return std::nullopt;
    }
    return glm::vec2(static_cast<float>(v[0].asReal()), static_cast<float>(v[1].asReal()));
}

std::optional<glm::vec3> vec3(const llsd::Value& v)
{
    if(!v.isArray() || v.size() < 3)
    {
        return std::nullopt;
    }
    return glm::vec3(static_cast<float>(v[0].asReal()), static_cast<float>(v[1].asReal()),
                     static_cast<float>(v[2].asReal()));
}

std::optional<glm::vec4> vec4(const llsd::Value& v)
{
    if(!v.isArray() || v.size() < 4)
    {
        return std::nullopt;
    }
    return glm::vec4(static_cast<float>(v[0].asReal()), static_cast<float>(v[1].asReal()),
                     static_cast<float>(v[2].asReal()), static_cast<float>(v[3].asReal()));
}

bool isNumber(const llsd::Value& v)
{
    return v.type() == llsd::Value::Type::Real || v.type() == llsd::Value::Type::Integer;
}

// glTF texture info -> the image URI's asset id, and its transform.
void readTexture(const llsd::Value& model, const llsd::Value& info, Uuid& id,
                 TextureTransform& transform)
{
    if(!info.isMap() || !info.has("index"))
    {
        return;
    }
    const llsd::Value& texture = model["textures"][static_cast<size_t>(info["index"].asInteger())];
    const llsd::Value& image = model["images"][static_cast<size_t>(texture["source"].asInteger())];
    id = Uuid::parse(image["uri"].asString()).value_or(Uuid{});
    const llsd::Value& t = info["extensions"]["KHR_texture_transform"];
    if(t.isMap())
    {
        transform.offset = vec2(t["offset"]).value_or(glm::vec2(0.0f));
        transform.scale = vec2(t["scale"]).value_or(glm::vec2(1.0f));
        transform.rotation = static_cast<float>(t["rotation"].asReal());
    }
}

} // namespace

MaterialOverride MaterialOverride::fromLlsd(const llsd::Value& data)
{
    MaterialOverride o;
    const llsd::Value& tex = data["tex"];
    for(size_t i = 0; i < kMaterialTextureCount && i < tex.size(); i++)
    {
        const Uuid id = tex[i].asUuid();
        if(!id.isNull())
        {
            o.textures[i] = id;
        }
    }
    o.baseColor = vec4(data["bc"]);
    o.emissive = vec3(data["ec"]);
    if(isNumber(data["mf"]))
    {
        o.metallic = static_cast<float>(data["mf"].asReal());
    }
    if(isNumber(data["rf"]))
    {
        o.roughness = static_cast<float>(data["rf"].asReal());
    }
    if(data["am"].type() == llsd::Value::Type::Integer)
    {
        o.alphaMode = static_cast<MaterialAlpha>(std::clamp(data["am"].asInteger(), 0, 2));
    }
    if(isNumber(data["ac"]))
    {
        o.alphaCutoff = static_cast<float>(data["ac"].asReal());
    }
    if(data["ds"].type() == llsd::Value::Type::Boolean ||
       data["ds"].type() == llsd::Value::Type::Integer)
    {
        o.doubleSided = data["ds"].asBoolean();
    }
    const llsd::Value& ti = data["ti"];
    for(size_t i = 0; i < kMaterialTextureCount && i < ti.size(); i++)
    {
        o.offsets[i] = vec2(ti[i]["o"]);
        o.scales[i] = vec2(ti[i]["s"]);
        if(isNumber(ti[i]["r"]))
        {
            o.rotations[i] = static_cast<float>(ti[i]["r"].asReal());
        }
    }
    return o;
}

Material applyOverride(Material m, const MaterialOverride& o)
{
    for(size_t i = 0; i < kMaterialTextureCount; i++)
    {
        if(o.textures[i])
        {
            m.textures[i] = *o.textures[i] == kOverrideNullTexture ? Uuid{} : *o.textures[i];
        }
        m.transforms[i].offset = o.offsets[i].value_or(m.transforms[i].offset);
        m.transforms[i].scale = o.scales[i].value_or(m.transforms[i].scale);
        m.transforms[i].rotation = o.rotations[i].value_or(m.transforms[i].rotation);
    }
    m.baseColor = o.baseColor.value_or(m.baseColor);
    m.emissive = o.emissive.value_or(m.emissive);
    m.metallic = o.metallic.value_or(m.metallic);
    m.roughness = o.roughness.value_or(m.roughness);
    m.alphaMode = o.alphaMode.value_or(m.alphaMode);
    m.alphaCutoff = o.alphaCutoff.value_or(m.alphaCutoff);
    m.doubleSided = o.doubleSided.value_or(m.doubleSided);
    return m;
}

std::expected<Material, std::string> parseGltfMaterial(std::string_view json)
{
    auto model = llsd::fromJson(json);
    if(!model)
    {
        return std::unexpected(model.error());
    }
    const llsd::Value& material = (*model)["materials"][0];
    if(!material.isMap())
    {
        return std::unexpected(std::string("no material in the glTF"));
    }
    Material m;
    const llsd::Value& pbr = material["pbrMetallicRoughness"];
    readTexture(*model, pbr["baseColorTexture"], m.textures[kBaseColorTexture],
                m.transforms[kBaseColorTexture]);
    readTexture(*model, material["normalTexture"], m.textures[kNormalTexture],
                m.transforms[kNormalTexture]);
    readTexture(*model, pbr["metallicRoughnessTexture"], m.textures[kMetallicRoughnessTexture],
                m.transforms[kMetallicRoughnessTexture]);
    readTexture(*model, material["emissiveTexture"], m.textures[kEmissiveTexture],
                m.transforms[kEmissiveTexture]);
    m.baseColor = vec4(pbr["baseColorFactor"]).value_or(glm::vec4(1.0f));
    m.emissive = vec3(material["emissiveFactor"]).value_or(glm::vec3(0.0f));
    if(pbr.has("metallicFactor"))
    {
        m.metallic = std::clamp(static_cast<float>(pbr["metallicFactor"].asReal()), 0.0f, 1.0f);
    }
    if(pbr.has("roughnessFactor"))
    {
        m.roughness = std::clamp(static_cast<float>(pbr["roughnessFactor"].asReal()), 0.0f, 1.0f);
    }
    const std::string alpha = material["alphaMode"].asString();
    m.alphaMode = alpha == "BLEND"  ? MaterialAlpha::Blend
                  : alpha == "MASK" ? MaterialAlpha::Mask
                                    : MaterialAlpha::Opaque;
    if(material.has("alphaCutoff"))
    {
        m.alphaCutoff =
            std::clamp(static_cast<float>(material["alphaCutoff"].asReal()), 0.0f, 1.0f);
    }
    m.doubleSided = material["doubleSided"].asBoolean();
    return m;
}

std::expected<Material, std::string> parseMaterialAsset(std::span<const uint8_t> bytes)
{
    auto asset = llsd::parse(llsd::asText(bytes));
    if(!asset)
    {
        // Binary LLSD without its header line.
        asset = llsd::fromBinary(bytes);
    }
    if(!asset)
    {
        return std::unexpected("material asset: " + asset.error());
    }
    const std::string version = (*asset)["version"].asString();
    if(version != "1.0" && version != "1.1")
    {
        return std::unexpected("material asset: unknown version " + version);
    }
    if((*asset)["type"].asString() != "GLTF 2.0")
    {
        return std::unexpected("material asset: not GLTF 2.0");
    }
    return parseGltfMaterial((*asset)["data"].asString());
}

} // namespace viewer::assets
