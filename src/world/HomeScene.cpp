#include "viewer/world/HomeScene.hpp"

#include "viewer/assets/Material.hpp"
#include "viewer/core/Md5.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>

namespace viewer::world
{

namespace
{

float real(const llsd::Value& v, float fallback = 0.0f)
{
    return v.isUndefined() ? fallback : static_cast<float>(v.asReal());
}

glm::vec3 vec3(const llsd::Value& v)
{
    return {real(v[0]), real(v[1]), real(v[2])};
}

glm::vec4 vec4(const llsd::Value& v, glm::vec4 fallback = glm::vec4(1.0f))
{
    if(!v.isArray() || v.size() < 4)
    {
        return fallback;
    }
    return {real(v[0]), real(v[1]), real(v[2]), real(v[3])};
}

// [x, y, z, w]; a zero (or missing) one is no rotation.
glm::quat quat(const llsd::Value& v)
{
    const glm::quat q(real(v[3]), real(v[0]), real(v[1]), real(v[2]));
    const float length = glm::length(q);
    return length > 1e-6f ? q / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// An id made from bytes (stable from run to run).
Uuid idOf(std::span<const uint8_t> bytes)
{
    Uuid id;
    id.bytes = md5(bytes);
    return id;
}

volume::VolumeParams shape(const llsd::Value& v)
{
    // LLVolumeParams::asLLSD's values: the same units as ours.
    volume::VolumeParams p;
    const llsd::Value& path = v["path"];
    p.path.curve = static_cast<uint8_t>(path["curve"].asInteger());
    p.path.begin = real(path["begin"]);
    p.path.end = real(path["end"], 1.0f);
    p.path.scale = {real(path["scale_x"], 1.0f), real(path["scale_y"], 1.0f)};
    p.path.shear = {real(path["shear_x"]), real(path["shear_y"])};
    p.path.twistBegin = real(path["twist_begin"]);
    p.path.twist = real(path["twist"]);
    p.path.radiusOffset = real(path["radius_offset"]);
    p.path.taper = {real(path["taper_x"]), real(path["taper_y"])};
    p.path.revolutions = real(path["revolutions"], 1.0f);
    p.path.skew = real(path["skew"]);
    const llsd::Value& profile = v["profile"];
    p.profile.curve = static_cast<uint8_t>(profile["curve"].asInteger());
    p.profile.begin = real(profile["begin"]);
    p.profile.end = real(profile["end"], 1.0f);
    p.profile.hollow = real(profile["hollow"]);
    return p;
}

// A face's legacy material, under the names RenderMaterials uses (the
// scene's are the same, starting in lower case).
assets::LegacyMaterial legacyMaterial(const llsd::Value& v)
{
    llsd::Value renamed{llsd::Map{}};
    if(const auto* map = v.get<llsd::Map>())
    {
        for(const auto& [key, value] : *map)
        {
            std::string name = key;
            if(!name.empty())
            {
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            }
            renamed[name] = value;
        }
    }
    return assets::legacyMaterialFromLlsd(renamed);
}

TextureFace face(const llsd::Value& v)
{
    TextureFace f;
    f.texture = v["imageid"].asUuid();
    f.color = vec4(v["color"]);
    f.repeats = {real(v["scales"], 1.0f), real(v["scalet"], 1.0f)};
    f.offset = {real(v["offsets"]), real(v["offsett"])};
    f.rotation = real(v["imagerot"]);
    // LLTextureEntry::asLLSD: bump is the bump-and-shiny byte, fullbright
    // apart.
    f.bumpShinyFullbright = static_cast<uint8_t>((v["bump"].asInteger() & 0xDF) |
                                                 (v["fullbright"].asInteger() ? 0x20 : 0));
    f.mediaTexGen = static_cast<uint8_t>(v["media_flags"].asInteger());
    f.glow = real(v["glow"]);
    return f;
}

ParticleSystem particles(const llsd::Value& v)
{
    ParticleSystem p;
    p.crc = static_cast<uint32_t>(v["pscrc"].asInteger());
    p.flags = static_cast<uint32_t>(v["psflags"].asInteger());
    p.pattern = static_cast<uint8_t>(v["pspattern"].asInteger());
    p.maxAge = real(v["psmaxage"]);
    p.startAge = real(v["psstartage"]);
    p.innerAngle = real(v["psinnerangle"]);
    p.outerAngle = real(v["psouterangle"]);
    p.burstRate = real(v["psburstrate"], 0.1f);
    p.burstRadius = real(v["psburstradius"]);
    p.burstSpeedMin = real(v["psburstspeedmin"], 1.0f);
    p.burstSpeedMax = real(v["psburstspeedmax"], 1.0f);
    p.burstCount = static_cast<uint8_t>(std::clamp(v["psburstpartcount"].asInteger(), 0, 255));
    p.angularVelocity = {real(v["psangvelx"]), real(v["psangvely"]), real(v["psangvelz"])};
    p.acceleration = {real(v["psaccelx"]), real(v["psaccely"]), real(v["psaccelz"])};
    p.texture = v["psuuid"].asUuid();
    p.target = v["pstargetuuid"].asUuid();
    p.particleFlags = static_cast<uint32_t>(v["pdflags"].asInteger());
    p.particleMaxAge = real(v["pdmaxage"], 10.0f);
    p.startColor = vec4(v["pdstartcolor"]);
    p.endColor = vec4(v["pdendcolor"]);
    p.startScale = {real(v["pdstartscalex"], 1.0f), real(v["pdstartscaley"], 1.0f)};
    p.endScale = {real(v["pdendscalex"], 1.0f), real(v["pdendscaley"], 1.0f)};
    return p;
}

void extraParameters(Object& o, const llsd::Value& v)
{
    if(const llsd::Value& f = v["flexible"]; f.isMap())
    {
        FlexibleParams flexible;
        flexible.tension = real(f["tension"], flexible.tension);
        flexible.drag = real(f["drag"], flexible.drag);
        flexible.gravity = real(f["gravity"], flexible.gravity);
        flexible.wind = real(f["wind"]);
        flexible.force = vec3(f["userforce"]);
        if(f.has("softness"))
        {
            flexible.softness = std::clamp(f["softness"].asInteger(), 0, 3);
        }
        o.flexible = flexible;
    }
    if(const llsd::Value& l = v["light"]; l.isMap())
    {
        // LLLightParams::asLLSD: linear color, alpha the intensity.
        const glm::vec4 color = vec4(l["color"]);
        LightParams light;
        light.color = glm::vec3(color);
        light.intensity = color.a;
        light.radius = real(l["radius"], light.radius);
        light.cutoff = real(l["cutoff"]);
        light.falloff = real(l["falloff"], light.falloff);
        o.light = light;
        // A projector: a texture, or its angle set (only lights project).
        const llsd::Value& image = v["lightimage"];
        const glm::vec3 params = vec3(image["params"]);
        if(image.isMap() && (!image["texture"].asUuid().isNull() || params != glm::vec3(0.0f)))
        {
            o.lightImage = LightImage{image["texture"].asUuid(), params.x, params.y, params.z};
        }
    }
    if(const llsd::Value& s = v["sculpted"]; s.isMap())
    {
        o.sculptTexture = s["texture"].asUuid();
        o.sculptType = static_cast<uint8_t>(s["type"].asInteger());
    }
}

} // namespace

std::expected<HomeScene, std::string> loadHomeScene(std::span<const uint8_t> bytes,
                                                    uint64_t regionHandle)
{
    auto parsed = llsd::fromBinary(bytes);
    if(!parsed)
    {
        return std::unexpected(std::format("not binary LLSD: {}", parsed.error()));
    }
    const llsd::Value& root = *parsed;
    if(root["_version"].asInteger() != 1)
    {
        return std::unexpected(std::format("version {}, not 1", root["_version"].asInteger()));
    }
    HomeScene out;
    out.assetUrl = root["asset_cdn"].asString();
    if(!out.assetUrl.empty() && out.assetUrl.find("://") == std::string::npos)
    {
        out.assetUrl = "http://" + out.assetUrl;
    }
    while(!out.assetUrl.empty() && out.assetUrl.back() == '/')
    {
        out.assetUrl.pop_back();
    }
    const llsd::Value& scene = root["scene"];

    // The terrain: its heights as 16 by 16 patches.
    const llsd::Value& terrain = scene["terrain"];
    const auto* heights = terrain["heights"].get<llsd::Binary>();
    constexpr size_t kSamples = size_t{kRegionWidth} * kRegionWidth;
    if(!heights || heights->size() != kSamples * sizeof(float))
    {
        return std::unexpected("the terrain's heights aren't 256 x 256 float32s");
    }
    {
        WorldUpdate::Terrain patches;
        constexpr uint32_t kPatches = kRegionWidth / kPatchSize;
        for(uint32_t py = 0; py < kPatches; py++)
        {
            for(uint32_t px = 0; px < kPatches; px++)
            {
                TerrainPatch patch{px, py, {}};
                for(uint32_t y = 0; y < kPatchSize; y++)
                {
                    for(uint32_t x = 0; x < kPatchSize; x++)
                    {
                        const size_t i =
                            size_t{py * kPatchSize + y} * kRegionWidth + px * kPatchSize + x;
                        uint32_t bits = 0;
                        std::memcpy(&bits, heights->data() + i * sizeof(float), sizeof(bits));
                        if constexpr(std::endian::native == std::endian::big)
                        {
                            bits = std::byteswap(bits);
                        }
                        patch.heights[y * kPatchSize + x] = std::bit_cast<float>(bits);
                    }
                }
                patches.patches.push_back(patch);
            }
        }
        out.updates.push_back({regionHandle, std::move(patches)});
    }
    {
        WorldUpdate::Handshake handshake;
        const llsd::Value& textures = terrain["textures"];
        for(size_t i = 0; i < 4; i++)
        {
            handshake.composition.detailTextures[i] = textures["detail"][i].asUuid();
            handshake.composition.startHeight[i] = real(textures["startHeights"][i]);
            handshake.composition.heightRange[i] = real(textures["heightRanges"][i]);
        }
        handshake.composition.waterHeight = real(terrain["water"], 20.0f);
        out.updates.push_back({regionHandle, handshake});
    }

    const llsd::Value& environment = scene["environment"];
    out.water = environment["water"].asUuid();
    for(size_t i = 0; i < environment["skies"].size(); i++)
    {
        if(environment["skies"][i]["height"].asInteger() == 0)
        {
            out.sky = environment["skies"][i]["sky"].asUuid();
        }
    }

    // The objects, as full updates: local ids in order, children after
    // their root.
    WorldUpdate::Objects objects;
    std::vector<WorldUpdate::MaterialOverrides> overrides;
    uint32_t nextId = 1;
    const llsd::Value& linksets = scene["objects"];
    for(size_t l = 0; l < linksets.size(); l++)
    {
        const llsd::Value& linkset = linksets[l];
        if(linkset.size() == 0)
        {
            continue;
        }
        // A marker ("#HOME:Name"): where something of the lobby's is,
        // never drawn.
        if(const std::string name = linkset[0]["name"].asString(); name.starts_with("#HOME:"))
        {
            const HomeMarker m{vec3(linkset[0]["pos"]), quat(linkset[0]["rotation"])};
            const std::string_view which = std::string_view(name).substr(6);
            if(which == "Floor")
            {
                out.floor = m.position.z;
            }
            else if(which == "Landing")
            {
                out.landing = m;
            }
            else if(which == "Login")
            {
                out.login = m;
            }
            else if(which == "News")
            {
                out.news = m;
            }
            else if(which == "GridStatus")
            {
                out.gridStatus = m;
            }
            continue;
        }
        const uint32_t rootId = nextId;
        for(size_t p = 0; p < linkset.size(); p++)
        {
            const llsd::Value& prim = linkset[p];
            ObjectUpdateData update;
            Object& o = update.full;
            o.localId = update.localId = nextId++;
            o.id.bytes = md5(std::format("home/{}/{}", l, p));
            o.region = regionHandle;
            o.type = ObjectType::Prim;
            o.parentId = p == 0 ? 0 : rootId;
            o.clickAction = static_cast<uint8_t>(prim["click_action"].asInteger());
            o.position = vec3(prim["pos"]);
            o.rotation = quat(prim["rotation"]);
            o.scale = vec3(prim["scale"]);
            o.hasVolume = true;
            o.volume = shape(prim["shape"]);
            extraParameters(o, prim["extra_parameters"]);
            if(const llsd::Value& ps = prim["particles"]; ps.isMap() && ps["pscrc"].asInteger())
            {
                o.particles = particles(ps);
            }

            // Faces past the list are as its first.
            const llsd::Value& faces = prim["facelist"];
            WorldUpdate::MaterialOverrides pbr;
            pbr.localId = o.localId;
            for(size_t f = 0; f < kMaxFaces; f++)
            {
                const size_t from = f < faces.size() ? f : 0;
                if(faces.size() == 0)
                {
                    break;
                }
                o.faces[f] = face(faces[from]);
                if(const llsd::Value& m = faces[from]["material"]; m.isMap())
                {
                    const assets::LegacyMaterial material = legacyMaterial(m);
                    o.faces[f].material = idOf(llsd::toBinary(m));
                    if(std::ranges::find(out.legacyMaterials, o.faces[f].material,
                                         &std::pair<Uuid, assets::LegacyMaterial>::first) ==
                       out.legacyMaterials.end())
                    {
                        out.legacyMaterials.emplace_back(o.faces[f].material, material);
                    }
                }
                if(f < faces.size())
                {
                    if(const llsd::Value& g = faces[f]["gltfMaterial"]; g.isMap())
                    {
                        o.materials.emplace_back(static_cast<uint8_t>(f), kHomeBlankMaterial);
                        pbr.faces.emplace_back(static_cast<uint8_t>(f),
                                               assets::MaterialOverride::fromLlsd(g));
                    }
                }
            }
            if(!pbr.faces.empty())
            {
                overrides.push_back(std::move(pbr));
            }
            objects.updates.push_back(std::move(update));
            objects.full++;
        }
    }
    out.updates.push_back({regionHandle, std::move(objects)});
    for(auto& o : overrides)
    {
        out.updates.push_back({regionHandle, std::move(o)});
    }
    return out;
}

} // namespace viewer::world
