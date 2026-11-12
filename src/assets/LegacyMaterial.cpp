#include "viewer/assets/LegacyMaterial.hpp"

#include "viewer/assets/MeshAsset.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

namespace viewer::assets
{

namespace
{

constexpr float kFixedPoint = 10000.0f; // MATERIALS_MULTIPLIER

float fixedPoint(const llsd::Value& data, std::string_view key)
{
    return static_cast<float>(data[key].asInteger()) / kFixedPoint;
}

float byte(const llsd::Value& data, std::string_view key)
{
    return static_cast<float>(std::clamp(data[key].asInteger(), 0, 255)) / 255.0f;
}

TextureTransform transform(const llsd::Value& data, std::string_view prefix)
{
    const std::string p(prefix);
    TextureTransform t;
    t.offset = glm::vec2(fixedPoint(data, p + "OffsetX"), fixedPoint(data, p + "OffsetY"));
    t.scale = glm::vec2(fixedPoint(data, p + "RepeatX"), fixedPoint(data, p + "RepeatY"));
    t.rotation = fixedPoint(data, p + "Rotation");
    return t;
}

Uuid uuidFromBinary(const llsd::Value& value)
{
    if(value.type() == llsd::Value::Type::Uuid)
    {
        return value.asUuid();
    }
    const llsd::Binary bytes = value.asBinary();
    Uuid id;
    if(bytes.size() == id.bytes.size())
    {
        std::memcpy(id.bytes.data(), bytes.data(), id.bytes.size());
    }
    return id;
}

} // namespace

LegacyMaterial legacyMaterialFromLlsd(const llsd::Value& data)
{
    LegacyMaterial m;
    m.normalMap = data["NormMap"].asUuid();
    m.normalTransform = transform(data, "Norm");
    m.specularMap = data["SpecMap"].asUuid();
    m.specularTransform = transform(data, "Spec");
    const llsd::Value& color = data["SpecColor"];
    if(color.isArray() && color.size() >= 3)
    {
        for(size_t i = 0; i < 4; i++)
        {
            m.specularColor[static_cast<int>(i)] =
                i < color.size()
                    ? static_cast<float>(std::clamp(color[i].asInteger(), 0, 255)) / 255.0f
                    : 1.0f;
        }
    }
    m.glossiness = byte(data, "SpecExp");
    m.environment = byte(data, "EnvIntensity");
    m.alphaMode =
        static_cast<DiffuseAlphaMode>(std::clamp(data["DiffuseAlphaMode"].asInteger(), 0, 4));
    m.alphaCutoff = byte(data, "AlphaMaskCutoff");
    return m;
}

llsd::Value legacyMaterialRequest(std::span<const Uuid> ids)
{
    llsd::Value list = llsd::Value(llsd::Array{});
    for(const Uuid& id : ids)
    {
        list.append(llsd::Value(llsd::Binary(id.bytes.begin(), id.bytes.end())));
    }
    llsd::Value body;
    body["Zipped"] = llsd::Value(deflateZlib(llsd::toBinary(list)));
    return body;
}

std::expected<std::vector<std::pair<Uuid, LegacyMaterial>>, std::string>
parseLegacyMaterials(const llsd::Value& response)
{
    const llsd::Binary zipped = response["Zipped"].asBinary();
    if(zipped.empty())
    {
        return std::unexpected(std::string("no Zipped field"));
    }
    auto inflated = inflateZlib(zipped);
    if(!inflated)
    {
        return std::unexpected(inflated.error());
    }
    std::span<const uint8_t> bytes = *inflated;
    // Some servers put the binary LLSD header first.
    constexpr std::string_view kHeader = "<? LLSD/Binary ?>\n";
    if(bytes.size() >= kHeader.size() &&
       std::memcmp(bytes.data(), kHeader.data(), kHeader.size()) == 0)
    {
        bytes = bytes.subspan(kHeader.size());
    }
    auto list = llsd::fromBinary(bytes);
    if(!list)
    {
        return std::unexpected(list.error());
    }
    if(!list->isArray())
    {
        return std::unexpected(std::string("not a list of materials"));
    }
    std::vector<std::pair<Uuid, LegacyMaterial>> out;
    for(size_t i = 0; i < list->size(); i++)
    {
        const llsd::Value& entry = (*list)[i];
        const Uuid id = uuidFromBinary(entry["ID"]);
        if(id.isNull() || !entry["Material"].isMap())
        {
            continue;
        }
        out.emplace_back(id, legacyMaterialFromLlsd(entry["Material"]));
    }
    return out;
}

std::vector<uint8_t> deflateZlib(std::span<const uint8_t> data)
{
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> out(size);
    if(compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()),
                 Z_BEST_COMPRESSION) != Z_OK)
    {
        return {};
    }
    out.resize(size);
    return out;
}

} // namespace viewer::assets
