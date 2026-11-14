#include "viewer/world/World.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/protocol/Messages.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace viewer::world
{

namespace msg = protocol::msg;

namespace
{

// Something to change in place: shared with a copy of the world, it gets its
// own copy first. (Only the owning thread makes new references, so a count
// of one stays one.)
template <typename T>
T& writable(std::shared_ptr<T>& p)
{
    if(!p)
    {
        p = std::make_shared<T>();
    }
    else if(p.use_count() > 1)
    {
        p = std::make_shared<T>(*p);
    }
    return *p;
}

} // namespace

RegionTerrain& World::region(uint64_t handle)
{
    auto [it, inserted] = terrain_.try_emplace(handle);
    RegionTerrain& terrain = writable(it->second);
    terrain.handle = handle;
    return terrain;
}

const RegionTerrain* World::terrain(uint64_t regionHandle) const
{
    const auto it = terrain_.find(regionHandle);
    return it == terrain_.end() ? nullptr : it->second.get();
}

void World::removeRegion(uint64_t regionHandle)
{
    terrain_.erase(regionHandle);
    std::erase_if(objects_, [&](const auto& entry) { return entry.first.region == regionHandle; });
    std::erase_if(ids_.keys,
                  [&](const auto& entry) { return entry.second.region == regionHandle; });
    std::erase_if(materialOverrides_,
                  [&](const auto& entry) { return entry.first.region == regionHandle; });
    objectsRevision_++;
}

float RegionTerrain::heightAt(float x, float y) const
{
    constexpr float kLast = static_cast<float>(kRegionWidth) - 1.001f;
    const float fx = std::clamp(x, 0.0f, kLast);
    const float fy = std::clamp(y, 0.0f, kLast);
    const auto x0 = static_cast<uint32_t>(fx);
    const auto y0 = static_cast<uint32_t>(fy);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float a = height(x0, y0) + (height(x0 + 1, y0) - height(x0, y0)) * tx;
    const float b = height(x0, y0 + 1) + (height(x0 + 1, y0 + 1) - height(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

const FaceOverrides* World::materialOverrides(const ObjectKey& key) const
{
    const auto it = materialOverrides_.find(key);
    return it == materialOverrides_.end() ? nullptr : it->second.get();
}

std::optional<WorldUpdate::MaterialOverrides> decodeMaterialOverrides(std::string_view notation)
{
    // {id: local id, te: [faces], od: [override per face]} (llgltfmateriallist.cpp
    // applyOverrideMessage).
    const auto data = llsd::fromNotation(notation);
    if(!data || !(*data)["te"].isArray())
    {
        return std::nullopt;
    }
    WorldUpdate::MaterialOverrides out;
    out.localId = static_cast<uint32_t>((*data)["id"].asInteger());
    const llsd::Value& faces = (*data)["te"];
    const llsd::Value& overrides = (*data)["od"];
    for(size_t i = 0; i < faces.size() && i < kMaxFaces; i++)
    {
        const int face = faces[i].asInteger();
        if(face >= 0 && static_cast<size_t>(face) < kMaxFaces)
        {
            out.faces.emplace_back(static_cast<uint8_t>(face),
                                   assets::MaterialOverride::fromLlsd(overrides[i]));
        }
    }
    out.raw = std::string(notation);
    return out;
}

const std::vector<World::PlayingAnimation>* World::animations(const Uuid& id) const
{
    const auto it = animations_.find(id);
    return it == animations_.end() ? nullptr : it->second.get();
}

const World::Appearance* World::appearance(const Uuid& avatar) const
{
    const auto it = appearances_.find(avatar);
    return it == appearances_.end() ? nullptr : it->second.get();
}

const Object* World::object(uint64_t region, uint32_t localId) const
{
    const auto it = objects_.find(ObjectKey{region, localId});
    return it == objects_.end() ? nullptr : it->second.get();
}

void World::applyObject(uint64_t regionHandle, const ObjectUpdateData& update,
                        std::chrono::steady_clock::time_point received, float timeDilation)
{
    const ObjectKey key{regionHandle, update.localId};
    if(update.kind == ObjectUpdateData::Kind::Terse)
    {
        // Terse updates only move known objects. (The official viewer asks
        // for the full update of unknown ones; we will too, later.)
        const auto it = objects_.find(key);
        if(it == objects_.end())
        {
            return;
        }
        Object& object = writable(it->second);
        applyUpdate(object, update);
        object.updatedAt = received;
        object.timeDilation = timeDilation;
    }
    else
    {
        if(!update.full.id.isNull())
        {
            auto [it, inserted] = ids().keys.try_emplace(update.full.id, key);
            if(!inserted && it->second != key)
            {
                // It moved (crossed into this region): drop the old copy.
                const ObjectKey old = std::exchange(it->second, key);
                removeWithChildren(old);
            }
        }
        Object& object = writable(objects_[key]);
        applyUpdate(object, update);
        object.region = regionHandle;
        object.updatedAt = received;
        object.timeDilation = timeDilation;
    }
}

World::IdIndex& World::ids()
{
    if(!ids_.valid)
    {
        ids_.keys.clear();
        for(const auto& [key, object] : objects_)
        {
            ids_.keys[object->id] = key;
        }
        ids_.valid = true;
    }
    return ids_;
}

void World::forgetId(const ObjectKey& key, const Object& object)
{
    IdIndex& index = ids();
    if(const auto it = index.keys.find(object.id); it != index.keys.end() && it->second == key)
    {
        index.keys.erase(it);
    }
}

void World::removeWithChildren(const ObjectKey& key)
{
    // Children first found by parent: an avatar's attachments, then their
    // linked prims (a few levels at most).
    std::vector<uint32_t> removed{key.localId};
    for(size_t first = 0; first < removed.size();)
    {
        const size_t end = removed.size();
        for(const auto& [k, object] : objects_)
        {
            if(k.region == key.region && object->parentId != 0 &&
               std::find(removed.begin() + static_cast<ptrdiff_t>(first), removed.end(),
                         object->parentId) != removed.end() &&
               std::find(removed.begin(), removed.end(), k.localId) == removed.end())
            {
                removed.push_back(k.localId);
            }
        }
        first = end;
    }
    for(const uint32_t localId : removed)
    {
        const ObjectKey k{key.region, localId};
        if(const auto it = objects_.find(k); it != objects_.end())
        {
            forgetId(k, *it->second);
            objects_.erase(it);
        }
    }
    objectsRevision_++;
}

namespace
{

WorldUpdate::Objects decodeObjectUpdate(const protocol::CircuitEvent& event)
{
    WorldUpdate::Objects out;
    const auto m = event.decode<msg::ObjectUpdate>();
    if(!m)
    {
        return out;
    }
    out.timeDilation = static_cast<float>(m->RegionData.TimeDilation) / 65535.0f;
    out.received = event.received;
    for(const auto& b : m->ObjectData)
    {
        out.full++;
        ObjectUpdateData update;
        update.kind = ObjectUpdateData::Kind::Full;
        update.localId = b.ID;
        Object& o = update.full;
        o.id = b.FullID;
        o.localId = b.ID;
        o.type = b.PCode;
        o.state = b.State;
        // Trees and grass: their species (llvotree.cpp, llvograss.cpp).
        o.species = b.Data.empty() ? 0 : b.Data[0];
        o.parentId = b.ParentID;
        o.updateFlags = b.UpdateFlags;
        o.clickAction = b.ClickAction;
        o.scale = glm::vec3(b.Scale.x, b.Scale.y, b.Scale.z);

        // Motion: 60 bytes (position, velocity, acceleration, rotation as
        // x/y/z, angular velocity), after a 16-byte foot plane for avatars.
        std::span<const uint8_t> motion(b.ObjectData);
        if(motion.size() == 76 || motion.size() == 140)
        {
            motion = motion.subspan(16);
        }
        if(motion.size() >= 60)
        {
            float f[15];
            std::memcpy(f, motion.data(), sizeof(f)); // little-endian floats
            o.position = glm::vec3(f[0], f[1], f[2]);
            o.velocity = glm::vec3(f[3], f[4], f[5]);
            o.acceleration = glm::vec3(f[6], f[7], f[8]);
            const glm::vec3 r(f[9], f[10], f[11]);
            const float w2 = 1.0f - glm::dot(r, r);
            o.rotation = glm::quat(w2 > 0.0f ? std::sqrt(w2) : 0.0f, r.x, r.y, r.z);
            o.angularVelocity = glm::vec3(f[12], f[13], f[14]);
        }
        applyExtraParams(o, b.ExtraParams);
        o.hoverText = protocol::toString(b.Text);
        o.hoverTextColor = b.TextColor;
        o.hoverTextColor[3] = static_cast<uint8_t>(255 - o.hoverTextColor[3]); // sent flipped
        if(o.type == ObjectType::Prim)
        {
            volume::PackedVolumeParams p;
            p.pathCurve = b.PathCurve;
            p.pathBegin = b.PathBegin;
            p.pathEnd = b.PathEnd;
            p.pathScaleX = b.PathScaleX;
            p.pathScaleY = b.PathScaleY;
            p.pathShearX = static_cast<int8_t>(b.PathShearX);
            p.pathShearY = static_cast<int8_t>(b.PathShearY);
            p.pathTwist = b.PathTwist;
            p.pathTwistBegin = b.PathTwistBegin;
            p.pathRadiusOffset = b.PathRadiusOffset;
            p.pathTaperX = b.PathTaperX;
            p.pathTaperY = b.PathTaperY;
            p.pathRevolutions = b.PathRevolutions;
            p.pathSkew = b.PathSkew;
            p.profileCurve = b.ProfileCurve;
            p.profileBegin = b.ProfileBegin;
            p.profileEnd = b.ProfileEnd;
            p.profileHollow = b.ProfileHollow;
            o.volume = volume::unpackVolumeParams(p);
            o.hasVolume = true;
        }
        if(auto faces = decodeTextureEntry(b.TextureEntry))
        {
            o.faces = *faces;
        }
        out.updates.push_back(std::move(update));
    }
    return out;
}

WorldUpdate::Objects decodeCompressedUpdate(const protocol::CircuitEvent& event)
{
    WorldUpdate::Objects out;
    const auto m = event.decode<msg::ObjectUpdateCompressed>();
    if(!m)
    {
        return out;
    }
    out.timeDilation = static_cast<float>(m->RegionData.TimeDilation) / 65535.0f;
    out.received = event.received;
    for(const auto& block : m->ObjectData)
    {
        out.compressed++;
        if(auto update = decodeCompressedObject(block.Data, block.UpdateFlags))
        {
            if(!(block.UpdateFlags & kFlagTemporaryOnRez))
            {
                update->compressed = block.Data;
            }
            out.updates.push_back(std::move(*update));
        }
        else
        {
            out.undecodable++;
        }
    }
    return out;
}

WorldUpdate::Objects decodeTerseUpdate(const protocol::CircuitEvent& event)
{
    WorldUpdate::Objects out;
    const auto m = event.decode<msg::ImprovedTerseObjectUpdate>();
    if(!m)
    {
        return out;
    }
    out.timeDilation = static_cast<float>(m->RegionData.TimeDilation) / 65535.0f;
    out.received = event.received;
    for(const auto& block : m->ObjectData)
    {
        out.terse++;
        if(auto update = decodeTerseObject(block.Data, block.TextureEntry))
        {
            out.updates.push_back(std::move(*update));
        }
        else
        {
            out.undecodable++;
        }
    }
    return out;
}

} // namespace

WorldUpdate decodeMessage(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    WorldUpdate out;
    out.region = regionHandle;
    if(event.is<msg::ObjectUpdateCompressed>())
    {
        out.change = decodeCompressedUpdate(event);
    }
    else if(event.is<msg::ImprovedTerseObjectUpdate>())
    {
        out.change = decodeTerseUpdate(event);
    }
    else if(event.is<msg::ObjectUpdate>())
    {
        out.change = decodeObjectUpdate(event);
    }
    else if(event.is<msg::AvatarAnimation>())
    {
        if(const auto m = event.decode<msg::AvatarAnimation>())
        {
            WorldUpdate::Animations a{m->Sender.ID, {}};
            for(const auto& anim : m->AnimationList)
            {
                a.playing.push_back({anim.AnimID, anim.AnimSequenceID});
            }
            out.change = std::move(a);
        }
    }
    else if(event.is<msg::ObjectAnimation>())
    {
        // An animated object's: kept by its id like an avatar's, also before
        // the object itself is known (as llviewermessage.cpp does).
        if(const auto m = event.decode<msg::ObjectAnimation>())
        {
            WorldUpdate::Animations a{m->Sender.ID, {}};
            for(const auto& anim : m->AnimationList)
            {
                a.playing.push_back({anim.AnimID, anim.AnimSequenceID});
            }
            out.change = std::move(a);
        }
    }
    else if(event.is<msg::GenericStreamingMessage>())
    {
        constexpr uint16_t kGltfMaterialOverride = 0x4175;
        if(const auto m = event.decode<msg::GenericStreamingMessage>();
           m && m->MethodData.Method == kGltfMaterialOverride)
        {
            const auto& d = m->DataBlock.Data;
            if(auto overrides = decodeMaterialOverrides(
                   std::string_view(reinterpret_cast<const char*>(d.data()), d.size())))
            {
                out.change = std::move(*overrides);
            }
        }
    }
    else if(event.is<msg::AvatarAppearance>())
    {
        if(const auto m = event.decode<msg::AvatarAppearance>())
        {
            if(auto faces = decodeTextureEntry(m->ObjectData.TextureEntry))
            {
                WorldUpdate::Appearance a{m->Sender.ID, {}};
                a.appearance.faces = *faces;
                for(const auto& p : m->VisualParam)
                {
                    a.appearance.visualParams.push_back(p.ParamValue);
                }
                // No hover block: the region doesn't do hover (llvoavatar.cpp).
                if(!m->AppearanceHover.empty())
                {
                    const auto& h = m->AppearanceHover[0].HoverHeight;
                    a.appearance.hover = glm::vec3(h.x, h.y, h.z);
                }
                out.change = std::move(a);
            }
        }
    }
    else if(event.is<msg::KillObject>())
    {
        if(const auto m = event.decode<msg::KillObject>())
        {
            WorldUpdate::Kill kill;
            for(const auto& b : m->ObjectData)
            {
                kill.localIds.push_back(b.ID);
            }
            out.change = std::move(kill);
        }
    }
    else if(event.is<msg::LayerData>())
    {
        const auto layer = event.decode<msg::LayerData>();
        if(!layer)
        {
            return out;
        }
        const auto type = static_cast<LayerType>(layer->LayerID.Type);
        if(type != LayerType::Land && type != LayerType::LandExtended)
        {
            return out; // wind, cloud, water: not used yet
        }
        auto patches = decodeLandLayer(layer->LayerID.Type, layer->LayerData_.Data);
        if(!patches)
        {
            log::warn("bad LayerData: {}", patches.error());
            return out;
        }
        out.change = WorldUpdate::Terrain{std::move(*patches)};
    }
    else if(event.is<msg::RegionHandshake>())
    {
        const auto handshake = event.decode<msg::RegionHandshake>();
        if(!handshake)
        {
            return out;
        }
        const auto& info = handshake->RegionInfo;
        TerrainComposition c;
        c.detailTextures = {info.TerrainDetail0, info.TerrainDetail1, info.TerrainDetail2,
                            info.TerrainDetail3};
        c.startHeight = {info.TerrainStartHeight00, info.TerrainStartHeight01,
                         info.TerrainStartHeight10, info.TerrainStartHeight11};
        c.heightRange = {info.TerrainHeightRange00, info.TerrainHeightRange01,
                         info.TerrainHeightRange10, info.TerrainHeightRange11};
        c.waterHeight = info.WaterHeight;
        out.change = WorldUpdate::Handshake{c};
    }
    return out;
}

void World::handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    apply(decodeMessage(regionHandle, event));
}

void World::apply(WorldUpdate update)
{
    const uint64_t regionHandle = update.region;
    if(auto* objects = std::get_if<WorldUpdate::Objects>(&update.change))
    {
        stats_.fullUpdates += objects->full;
        stats_.compressedUpdates += objects->compressed;
        stats_.terseUpdates += objects->terse;
        stats_.undecodable += objects->undecodable;
        for(const ObjectUpdateData& u : objects->updates)
        {
            applyObject(regionHandle, u, objects->received, objects->timeDilation);
        }
        objectsRevision_ += !objects->updates.empty();
    }
    else if(auto* kill = std::get_if<WorldUpdate::Kill>(&update.change))
    {
        for(const uint32_t id : kill->localIds)
        {
            const ObjectKey key{regionHandle, id};
            if(const auto it = objects_.find(key); it != objects_.end())
            {
                forgetId(key, *it->second);
                objects_.erase(it);
            }
            materialOverrides_.erase(key);
        }
        objectsRevision_++;
    }
    else if(auto* overrides = std::get_if<WorldUpdate::MaterialOverrides>(&update.change))
    {
        const ObjectKey key{regionHandle, overrides->localId};
        if(overrides->faces.empty())
        {
            materialOverrides_.erase(key);
        }
        else
        {
            materialOverrides_[key] =
                std::make_shared<const FaceOverrides>(std::move(overrides->faces));
        }
        objectsRevision_++;
    }
    else if(auto* animations = std::get_if<WorldUpdate::Animations>(&update.change))
    {
        animations_[animations->avatar] =
            std::make_shared<const std::vector<PlayingAnimation>>(std::move(animations->playing));
    }
    else if(auto* appearance = std::get_if<WorldUpdate::Appearance>(&update.change))
    {
        Appearance a = std::move(appearance->appearance);
        a.serial = ++appearanceSerial_;
        appearances_[appearance->avatar] = std::make_shared<const Appearance>(std::move(a));
        objectsRevision_++;
    }
    else if(auto* terrainUpdate = std::get_if<WorldUpdate::Terrain>(&update.change))
    {
        RegionTerrain& terrain = region(regionHandle);
        constexpr uint32_t kPatches = kRegionWidth / kPatchSize;
        for(const TerrainPatch& patch : terrainUpdate->patches)
        {
            if(patch.x >= kPatches || patch.y >= kPatches)
            {
                continue; // larger (variable-size) regions aren't supported yet
            }
            for(uint32_t y = 0; y < kPatchSize; y++)
            {
                for(uint32_t x = 0; x < kPatchSize; x++)
                {
                    terrain.heights[(patch.y * kPatchSize + y) * kRegionWidth +
                                    patch.x * kPatchSize + x] = patch.heights[y * kPatchSize + x];
                }
            }
            terrain.patchesReceived.set(patch.y * kPatches + patch.x);
        }
        terrain.revision++;
    }
    else if(auto* handshake = std::get_if<WorldUpdate::Handshake>(&update.change))
    {
        RegionTerrain& terrain = region(regionHandle);
        terrain.composition = handshake->composition;
        terrain.hasComposition = true;
        terrain.revision++;
    }
    else if(std::holds_alternative<WorldUpdate::RegionRemoved>(update.change))
    {
        removeRegion(regionHandle);
    }
}

} // namespace viewer::world
