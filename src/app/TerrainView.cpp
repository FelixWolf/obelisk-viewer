#include "TerrainView.hpp"

#include "viewer/world/TerrainLayers.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

#include <cmath>

namespace viewer
{

namespace
{

constexpr uint32_t kWidth = world::kRegionWidth;
constexpr uint32_t kVertices = kWidth + 1;

int64_t regionX(uint64_t handle)
{
    return static_cast<int64_t>(handle >> 32);
}

int64_t regionY(uint64_t handle)
{
    return static_cast<int64_t>(handle & 0xFFFFFFFFu);
}

uint64_t makeHandle(int64_t x, int64_t y)
{
    return static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(y);
}

bool hasPatch(const world::RegionTerrain& t, uint32_t x, uint32_t y)
{
    constexpr uint32_t kPatches = kWidth / world::kPatchSize;
    return t.patchesReceived.test((y / world::kPatchSize) * kPatches + x / world::kPatchSize);
}

// The regions east, north and north-east of `handle`, whose samples the
// far edges use.
std::array<uint64_t, 3> edgeNeighbours(uint64_t handle)
{
    const int64_t x = regionX(handle);
    const int64_t y = regionY(handle);
    return {makeHandle(x + kWidth, y), makeHandle(x, y + kWidth),
            makeHandle(x + kWidth, y + kWidth)};
}

} // namespace

TerrainMeshData buildTerrainMesh(const world::RegionTerrain& region, const RegionLookup& lookup)
{
    const int64_t originX = regionX(region.handle);
    const int64_t originY = regionY(region.handle);

    // The region (if known, with that sample received) owning local sample
    // (x, y), which may lie outside this region by up to one row/column.
    auto owner = [&](int64_t x, int64_t y) -> std::pair<const world::RegionTerrain*, glm::uvec2>
    {
        const int64_t gx = originX + x;
        const int64_t gy = originY + y;
        if(gx < 0 || gy < 0)
        {
            return {nullptr, {}};
        }
        const int64_t rx = gx / kWidth * kWidth;
        const int64_t ry = gy / kWidth * kWidth;
        const world::RegionTerrain* t =
            rx == originX && ry == originY ? &region : lookup(makeHandle(rx, ry));
        const glm::uvec2 local(static_cast<uint32_t>(gx - rx), static_cast<uint32_t>(gy - ry));
        if(!t || !hasPatch(*t, local.x, local.y))
        {
            return {nullptr, {}};
        }
        return {t, local};
    };

    // Heights with a one-sample border, for the shared edge and normals.
    // Unknown samples fall back to the nearest one of this region.
    constexpr int64_t kBorder = 1;
    constexpr int64_t kGrid = kVertices + 2 * kBorder;
    std::vector<float> grid(static_cast<size_t>(kGrid * kGrid));
    for(int64_t y = -kBorder; y < kVertices + kBorder; y++)
    {
        for(int64_t x = -kBorder; x < kVertices + kBorder; x++)
        {
            float h = 0.0f;
            if(x >= 0 && y >= 0 && x < kWidth && y < kWidth)
            {
                h = region.height(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            }
            else if(const auto [t, local] = owner(x, y); t)
            {
                h = t->height(local.x, local.y);
            }
            else
            {
                h = region.height(static_cast<uint32_t>(std::clamp<int64_t>(x, 0, kWidth - 1)),
                                  static_cast<uint32_t>(std::clamp<int64_t>(y, 0, kWidth - 1)));
            }
            grid[static_cast<size_t>((y + kBorder) * kGrid + x + kBorder)] = h;
        }
    }
    auto heightAt = [&](int64_t x, int64_t y)
    { return grid[static_cast<size_t>((y + kBorder) * kGrid + x + kBorder)]; };

    const std::vector<float> layers = world::terrainLayerValues(region);
    auto ownLayer = [&](int64_t x, int64_t y)
    {
        return layers[static_cast<size_t>(std::min<int64_t>(y, kWidth - 1) * kWidth +
                                          std::min<int64_t>(x, kWidth - 1))];
    };
    // On the far edges, use the neighbour's value, shifted by whole layers to
    // within 1 of ours (neighbours may use different layer settings), as the
    // official viewer does (llviewerregion.cpp getCompositionXY).
    auto layerAt = [&](int64_t x, int64_t y)
    {
        const float own = ownLayer(x, y);
        if(x < kWidth && y < kWidth)
        {
            return own;
        }
        const auto [t, local] = owner(x, y);
        if(!t)
        {
            return own;
        }
        float adjacent = world::terrainLayerValue(*t, local.x, local.y);
        while(std::abs(own - adjacent) >= 1.0f)
        {
            adjacent += own > adjacent ? 1.0f : -1.0f;
        }
        return adjacent;
    };

    TerrainMeshData mesh;
    mesh.vertices.reserve(kVertices * kVertices);
    for(int64_t y = 0; y < kVertices; y++)
    {
        for(int64_t x = 0; x < kVertices; x++)
        {
            const float h = heightAt(x, y);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(heightAt(x - 1, y) - heightAt(x + 1, y),
                                         heightAt(x, y - 1) - heightAt(x, y + 1), 2.0f));
            const float noise = world::terrainBlendNoise(static_cast<double>(originX + x),
                                                         static_cast<double>(originY + y));
            mesh.vertices.push_back({glm::vec3(static_cast<float>(x), static_cast<float>(y), h),
                                     normal, glm::vec2(layerAt(x, y), noise)});
        }
    }
    mesh.indices.reserve(kWidth * kWidth * 6);
    for(uint32_t y = 0; y < kWidth; y++)
    {
        for(uint32_t x = 0; x < kWidth; x++)
        {
            const uint32_t a = y * kVertices + x;
            const uint32_t c = a + kVertices;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, c + 1, a, c + 1, c});
        }
    }
    return mesh;
}

void TerrainView::update(const world::World& world, render::Renderer& renderer)
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(regions_, [&](const auto& entry) { return !world.terrain(entry.first); });
    const RegionLookup lookup = [&](uint64_t handle) { return world.terrain(handle); };

    for(const auto& [handle, terrain] : world.terrain())
    {
        if(terrain.patchesReceived.none())
        {
            continue;
        }
        // The far edges use the neighbours, so their changes count too.
        uint64_t revision = terrain.revision;
        for(const uint64_t neighbour : edgeNeighbours(handle))
        {
            if(const world::RegionTerrain* t = world.terrain(neighbour))
            {
                revision = revision * 31 + t->revision + 1;
            }
        }
        Region& region = regions_[handle];
        if(region.mesh && region.revision == revision)
        {
            continue;
        }
        // While patches stream in, rebuild at most twice a second.
        if(region.mesh && !terrain.complete() &&
           now - region.builtAt < std::chrono::milliseconds(500))
        {
            continue;
        }
        const TerrainMeshData data = buildTerrainMesh(terrain, lookup);
        region.mesh = renderer.createMesh(std::span<const render::TerrainVertex>(data.vertices),
                                          data.indices);
        region.revision = revision;
        const auto [low, high] = std::minmax_element(data.vertices.begin(), data.vertices.end(),
                                                     [](const auto& a, const auto& b)
                                                     { return a.position.z < b.position.z; });
        region.minHeight = low != data.vertices.end() ? low->position.z : 0.0f;
        region.maxHeight = high != data.vertices.end() ? high->position.z : 0.0f;
        region.builtAt = now;
    }
}

void TerrainView::addTo(render::Scene& scene, uint64_t originHandle,
                        const MaterialFor& materialFor) const
{
    for(const auto& [handle, region] : regions_)
    {
        if(!region.mesh)
        {
            continue;
        }
        const glm::vec3 offset(static_cast<float>(regionX(handle) - regionX(originHandle)),
                               static_cast<float>(regionY(handle) - regionY(originHandle)), 0.0f);
        constexpr float kWidth = static_cast<float>(world::kRegionWidth);
        // Out of view, hills may still shade what's in view, and reflection
        // probes see all around.
        const bool inView =
            scene.frustum.containsBox(offset + glm::vec3(0.0f, 0.0f, region.minHeight),
                                      offset + glm::vec3(kWidth, kWidth, region.maxHeight));
        scene.terrain.push_back(
            {&region.mesh, glm::translate(glm::mat4(1.0f), offset), materialFor(handle), !inView});
    }
}

} // namespace viewer
