#include "viewer/world/World.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/protocol/Messages.hpp"

#include <cmath>
#include <cstring>

namespace viewer::world
{

namespace msg = protocol::msg;

RegionTerrain& World::region(uint64_t handle)
{
    auto [it, inserted] = terrain_.try_emplace(handle);
    if(inserted)
    {
        it->second.handle = handle;
    }
    return it->second;
}

const RegionTerrain* World::terrain(uint64_t regionHandle) const
{
    const auto it = terrain_.find(regionHandle);
    return it == terrain_.end() ? nullptr : &it->second;
}

void World::removeRegion(uint64_t regionHandle)
{
    terrain_.erase(regionHandle);
    std::erase_if(objects_, [&](const auto& entry) { return entry.first.region == regionHandle; });
    objectsRevision_++;
}

const Object* World::object(uint64_t region, uint32_t localId) const
{
    const auto it = objects_.find(ObjectKey{region, localId});
    return it == objects_.end() ? nullptr : &it->second;
}

void World::apply(uint64_t regionHandle, const ObjectUpdateData& update)
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
        applyUpdate(it->second, update);
    }
    else
    {
        Object& object = objects_[key];
        applyUpdate(object, update);
        object.region = regionHandle;
    }
    objectsRevision_++;
}

void World::handleCompressedUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    const auto m = event.decode<msg::ObjectUpdateCompressed>();
    if(!m)
    {
        return;
    }
    for(const auto& block : m->ObjectData)
    {
        if(auto update = decodeCompressedObject(block.Data, block.UpdateFlags))
        {
            apply(regionHandle, *update);
        }
    }
}

void World::handleTerseUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    const auto m = event.decode<msg::ImprovedTerseObjectUpdate>();
    if(!m)
    {
        return;
    }
    for(const auto& block : m->ObjectData)
    {
        if(auto update = decodeTerseObject(block.Data, block.TextureEntry))
        {
            apply(regionHandle, *update);
        }
    }
}

void World::handleObjectUpdate(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    const auto m = event.decode<msg::ObjectUpdate>();
    if(!m)
    {
        return;
    }
    for(const auto& b : m->ObjectData)
    {
        ObjectUpdateData update;
        update.kind = ObjectUpdateData::Kind::Full;
        update.localId = b.ID;
        Object& o = update.full;
        o.id = b.FullID;
        o.localId = b.ID;
        o.type = b.PCode;
        o.state = b.State;
        o.parentId = b.ParentID;
        o.updateFlags = b.UpdateFlags;
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
        apply(regionHandle, update);
    }
}

void World::handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    if(event.is<msg::ObjectUpdateCompressed>())
    {
        handleCompressedUpdate(regionHandle, event);
    }
    else if(event.is<msg::ImprovedTerseObjectUpdate>())
    {
        handleTerseUpdate(regionHandle, event);
    }
    else if(event.is<msg::ObjectUpdate>())
    {
        handleObjectUpdate(regionHandle, event);
    }
    else if(event.is<msg::KillObject>())
    {
        if(const auto m = event.decode<msg::KillObject>())
        {
            for(const auto& b : m->ObjectData)
            {
                objects_.erase(ObjectKey{regionHandle, b.ID});
            }
            objectsRevision_++;
        }
    }
    else if(event.is<msg::LayerData>())
    {
        const auto layer = event.decode<msg::LayerData>();
        if(!layer)
        {
            return;
        }
        const auto type = static_cast<LayerType>(layer->LayerID.Type);
        if(type != LayerType::Land && type != LayerType::LandExtended)
        {
            return; // wind, cloud, water: not used yet
        }
        auto patches = decodeLandLayer(layer->LayerID.Type, layer->LayerData_.Data);
        if(!patches)
        {
            log::warn("bad LayerData: {}", patches.error());
            return;
        }
        RegionTerrain& terrain = region(regionHandle);
        constexpr uint32_t kPatches = kRegionWidth / kPatchSize;
        for(const TerrainPatch& patch : *patches)
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
    else if(event.is<msg::RegionHandshake>())
    {
        const auto handshake = event.decode<msg::RegionHandshake>();
        if(!handshake)
        {
            return;
        }
        const auto& info = handshake->RegionInfo;
        RegionTerrain& terrain = region(regionHandle);
        TerrainComposition& c = terrain.composition;
        c.detailTextures = {info.TerrainDetail0, info.TerrainDetail1, info.TerrainDetail2,
                            info.TerrainDetail3};
        c.startHeight = {info.TerrainStartHeight00, info.TerrainStartHeight01,
                         info.TerrainStartHeight10, info.TerrainStartHeight11};
        c.heightRange = {info.TerrainHeightRange00, info.TerrainHeightRange01,
                         info.TerrainHeightRange10, info.TerrainHeightRange11};
        c.waterHeight = info.WaterHeight;
        terrain.hasComposition = true;
        terrain.revision++;
    }
}

} // namespace viewer::world
