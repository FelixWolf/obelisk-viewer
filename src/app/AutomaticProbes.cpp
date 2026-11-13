#include "AutomaticProbes.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace viewer
{

namespace
{

constexpr float kCell = 32.0f;      // a probe's octree node, across
constexpr float kParent = 64.0f;    // the node that splits into them
constexpr uint32_t kCapacity = 128; // OctreeMaxNodeCapacity
constexpr float kMinimumRadius = 8.0f;
constexpr float kAboveGround = 2.0f;

struct CellKey
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool operator==(const CellKey&) const = default;
};

struct CellKeyHash
{
    size_t operator()(const CellKey& k) const noexcept
    {
        uint64_t h = static_cast<uint32_t>(k.x);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(k.y);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(k.z);
        return static_cast<size_t>(h);
    }
};

CellKey cellOf(const glm::vec3& p, float size)
{
    return {static_cast<int32_t>(std::floor(p.x / size)),
            static_cast<int32_t>(std::floor(p.y / size)),
            static_cast<int32_t>(std::floor(p.z / size))};
}

struct Bounds
{
    glm::vec3 min{1e30f};
    glm::vec3 max{-1e30f};
};

} // namespace

std::vector<render::ReflectionProbe>
placeAutomaticProbes(std::span<const ProbeObject> objects,
                     const std::function<float(glm::vec2)>& groundAt, uint64_t idSeed)
{
    // An object belongs to the smallest cell it fits (its sphere no wider
    // than the cell), by its center.
    std::unordered_map<CellKey, uint32_t, CellKeyHash> parents;
    for(const ProbeObject& o : objects)
    {
        if(2.0f * o.radius <= kParent)
        {
            parents[cellOf(o.center, kParent)]++;
        }
    }
    std::unordered_map<CellKey, Bounds, CellKeyHash> cells;
    for(const ProbeObject& o : objects)
    {
        if(2.0f * o.radius > kCell)
        {
            continue;
        }
        const auto parent = parents.find(cellOf(o.center, kParent));
        if(parent == parents.end() || parent->second <= kCapacity)
        {
            continue;
        }
        Bounds& b = cells[cellOf(o.center, kCell)];
        b.min = glm::min(b.min, o.center - glm::vec3(o.radius));
        b.max = glm::max(b.max, o.center + glm::vec3(o.radius));
    }

    std::vector<render::ReflectionProbe> probes;
    probes.reserve(cells.size());
    for(const auto& [key, b] : cells)
    {
        render::ReflectionProbe probe;
        probe.id = CellKeyHash{}(key) ^ idSeed;
        probe.origin = (b.min + b.max) * 0.5f;
        const float ground = groundAt(glm::vec2(probe.origin)) + kAboveGround;
        probe.origin.z = std::max(probe.origin.z, ground);
        // Reaching every corner of the bounds.
        float r2 = 0.0f;
        for(int i = 0; i < 8; i++)
        {
            const glm::vec3 corner((i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y,
                                   (i & 4) ? b.max.z : b.min.z);
            const glm::vec3 d = corner - probe.origin;
            r2 = std::max(r2, glm::dot(d, d));
        }
        probe.radius = std::max(std::sqrt(r2), kMinimumRadius);
        // The near clip (half the radius) shouldn't reach under the ground.
        probe.origin.z = std::max(probe.origin.z, ground + probe.radius * 0.5f);
        probe.nearClip = probe.radius * 0.5f;
        probes.push_back(probe);
    }
    return probes;
}

} // namespace viewer
