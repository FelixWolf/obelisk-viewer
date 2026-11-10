#include "viewer/world/World.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/protocol/Messages.hpp"

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
}

void World::handleMessage(uint64_t regionHandle, const protocol::CircuitEvent& event)
{
    if(event.is<msg::LayerData>())
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
