#include "TerrainView.hpp"

#include "Procedural.hpp"

#include <glm/gtc/matrix_transform.hpp>

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

// Bilinear over the four corners: southwest, northwest, southeast, northeast.
float corners(const std::array<float, 4>& c, float fx, float fy)
{
    const float south = glm::mix(c[0], c[2], fx);
    const float north = glm::mix(c[1], c[3], fx);
    return glm::mix(south, north, fy);
}

} // namespace

TerrainMeshData buildTerrainMesh(const world::RegionTerrain& region, const HeightSampler& heightAt)
{
    const int64_t originX = regionX(region.handle);
    const int64_t originY = regionY(region.handle);

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
            else if(auto neighbour = heightAt(originX + x, originY + y))
            {
                h = *neighbour;
            }
            else
            {
                h = region.height(static_cast<uint32_t>(std::clamp<int64_t>(x, 0, kWidth - 1)),
                                  static_cast<uint32_t>(std::clamp<int64_t>(y, 0, kWidth - 1)));
            }
            grid[static_cast<size_t>((y + kBorder) * kGrid + x + kBorder)] = h;
        }
    }
    auto at = [&](int64_t x, int64_t y)
    { return grid[static_cast<size_t>((y + kBorder) * kGrid + x + kBorder)]; };

    world::TerrainComposition composition = region.composition;
    if(!region.hasComposition)
    {
        composition.startHeight = {10.0f, 10.0f, 10.0f, 10.0f};
        composition.heightRange = {60.0f, 60.0f, 60.0f, 60.0f};
    }

    TerrainMeshData mesh;
    mesh.vertices.reserve(kVertices * kVertices);
    for(int64_t y = 0; y < kVertices; y++)
    {
        for(int64_t x = 0; x < kVertices; x++)
        {
            const float h = at(x, y);
            const glm::vec3 normal = glm::normalize(
                glm::vec3(at(x - 1, y) - at(x + 1, y), at(x, y - 1) - at(x, y + 1), 2.0f));

            // As the official viewer: (height + noise - start) * 4 / range.
            const float fx = static_cast<float>(x) / kWidth;
            const float fy = static_cast<float>(y) / kWidth;
            const float start = corners(composition.startHeight, fx, fy);
            const float range = std::max(corners(composition.heightRange, fx, fy), 0.1f);
            const float gx = static_cast<float>(originX + x);
            const float gy = static_cast<float>(originY + y);
            const float twiddle =
                (procedural::fbm(gx * 0.04f, gy * 0.04f, 3, 1 << 20) - 0.5f) * 10.0f;
            const float value = glm::clamp((h + twiddle - start) * 4.0f / range, 0.0f, 3.0f);

            mesh.vertices.push_back(
                {glm::vec3(static_cast<float>(x), static_cast<float>(y), h), normal, value});
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

    const HeightSampler heightAt = [&](int64_t gx, int64_t gy) -> std::optional<float>
    {
        if(gx < 0 || gy < 0)
        {
            return std::nullopt;
        }
        const uint64_t handle = static_cast<uint64_t>(gx / kWidth * kWidth) << 32 |
                                static_cast<uint64_t>(gy / kWidth * kWidth);
        const world::RegionTerrain* t = world.terrain(handle);
        if(!t)
        {
            return std::nullopt;
        }
        const auto lx = static_cast<uint32_t>(gx % kWidth);
        const auto ly = static_cast<uint32_t>(gy % kWidth);
        constexpr uint32_t kPatches = kWidth / world::kPatchSize;
        if(!t->patchesReceived.test((ly / world::kPatchSize) * kPatches + lx / world::kPatchSize))
        {
            return std::nullopt;
        }
        return t->height(lx, ly);
    };

    for(const auto& [handle, terrain] : world.terrain())
    {
        Region& region = regions_[handle];
        if(region.mesh && region.revision == terrain.revision)
        {
            continue;
        }
        // While patches stream in, rebuild at most twice a second.
        if(region.mesh && !terrain.complete() &&
           now - region.builtAt < std::chrono::milliseconds(500))
        {
            continue;
        }
        if(terrain.patchesReceived.none())
        {
            continue;
        }
        const TerrainMeshData data = buildTerrainMesh(terrain, heightAt);
        region.mesh = renderer.createMesh(std::span<const render::TerrainVertex>(data.vertices),
                                          data.indices);
        region.revision = terrain.revision;
        region.builtAt = now;
    }
}

void TerrainView::addTo(render::Scene& scene, uint64_t originHandle, const Material& material) const
{
    for(const auto& [handle, region] : regions_)
    {
        if(!region.mesh)
        {
            continue;
        }
        const glm::vec3 offset(static_cast<float>(regionX(handle) - regionX(originHandle)),
                               static_cast<float>(regionY(handle) - regionY(originHandle)), 0.0f);
        scene.terrain.push_back({&region.mesh, glm::translate(glm::mat4(1.0f), offset), material});
    }
}

} // namespace viewer
