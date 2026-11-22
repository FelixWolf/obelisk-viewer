#include "AutomaticProbes.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace viewer
{

namespace
{

constexpr float kCell = 32.0f; // a probe's octree node, across
// Shrink-wrapped objects' bin radius: a quarter of their radius
// (llvovolume.cpp getBinRadius).
constexpr float kShrinkWrap = 0.25f;
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

// The official octree's root starts centered on 0, 1 m from its middle to
// each side, and grows towards each object by moving its center one half
// size and doubling (lloctree.h LLOctreeRoot::insert, pushCenter): its
// centers stay at 2^k - 1, so its nodes' sides are at -1 m plus multiples
// of their size, not at multiples. At a cell's edge that's which objects
// a probe sits among.
constexpr float kGridOrigin = -1.0f;

CellKey cellOf(const glm::vec3& p, float size)
{
    const glm::vec3 q = (p - glm::vec3(kGridOrigin)) / size;
    return {static_cast<int32_t>(std::floor(q.x)), static_cast<int32_t>(std::floor(q.y)),
            static_cast<int32_t>(std::floor(q.z))};
}

// Half the size of the box around an object, along the axes.
glm::vec3 extentOf(const ProbeObject& o)
{
    if(o.half == glm::vec3(0.0f))
    {
        return glm::vec3(o.radius);
    }
    const glm::mat3 m = glm::mat3_cast(o.rotation);
    return glm::abs(m[0]) * o.half.x + glm::abs(m[1]) * o.half.y + glm::abs(m[2]) * o.half.z;
}

// Where along `from + t d` (t from 0 to 1) the segment first meets the
// object's box (from inside it: where it leaves); 1 if it doesn't.
float firstHit(const ProbeObject& o, const glm::vec3& from, const glm::vec3& d)
{
    const glm::vec3 half = o.half == glm::vec3(0.0f) ? glm::vec3(o.radius) : o.half;
    const glm::quat toLocal = glm::inverse(o.rotation);
    const glm::vec3 p = toLocal * (from - o.center);
    const glm::vec3 v = toLocal * d;
    float enter = -1e30f;
    float leave = 1e30f;
    for(int i = 0; i < 3; i++)
    {
        if(std::abs(v[i]) < 1e-9f)
        {
            if(std::abs(p[i]) > half[i])
            {
                return 1.0f;
            }
            continue;
        }
        float t0 = (-half[i] - p[i]) / v[i];
        float t1 = (half[i] - p[i]) / v[i];
        if(t0 > t1)
        {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
    }
    if(enter > leave)
    {
        return 1.0f;
    }
    const float t = enter >= 0.0f ? enter : leave;
    return t >= 0.0f && t < 1.0f ? t : 1.0f;
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
    // Every object whose bin radius is up to the cell's size settles in the
    // cell's node or below it, by its center.
    struct Cell
    {
        Bounds bounds;
        std::vector<const ProbeObject*> objects;
    };
    std::unordered_map<CellKey, Cell, CellKeyHash> cells;
    for(const ProbeObject& o : objects)
    {
        // Taken as shrink-wrapped: the official viewer shrink-wraps objects
        // whose texture entry, scale or the like changed after they were
        // made (llvovolume.cpp, llviewerobject.cpp shrinkWrap), which is
        // most of them in a session but depends on its history; big flat
        // prims like a skybox floor then count for the 32 m node they're in.
        // (Not shrink-wrapped, the radius is at least 3 m and only up to the
        // cell's size counts, which leaves such floors out.)
        if(o.radius * kShrinkWrap > kCell)
        {
            continue;
        }
        Cell& c = cells[cellOf(o.center, kCell)];
        // Its extents (the official spatial extents: the box around it).
        const glm::vec3 reach = extentOf(o);
        c.bounds.min = glm::min(c.bounds.min, o.center - reach);
        c.bounds.max = glm::max(c.bounds.max, o.center + reach);
        c.objects.push_back(&o);
    }

    std::vector<render::ReflectionProbe> probes;
    probes.reserve(cells.size());
    for(const auto& [key, cell] : cells)
    {
        const Bounds& b = cell.bounds;
        render::ReflectionProbe probe;
        probe.id = CellKeyHash{}(key) ^ idSeed;
        // From the middle of the bounds, towards each corner: where the ray
        // first meets one of the cell's objects, or the corner; the probe
        // goes to the middle of what those make (LLReflectionMap::
        // autoAdjustOrigin, nudging it towards the open space among them).
        // Objects are their boxes here (prims' surfaces lie on or inside
        // them).
        const glm::vec3 center = (b.min + b.max) * 0.5f;
        glm::vec3 lo = b.max;
        glm::vec3 hi = b.min;
        bool hit = false;
        for(int i = 0; i < 8; i++)
        {
            const glm::vec3 corner((i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y,
                                   (i & 4) ? b.max.z : b.min.z);
            const glm::vec3 d = corner - center;
            float nearest = 1.0f;
            for(const ProbeObject* o : cell.objects)
            {
                nearest = std::min(nearest, firstHit(*o, center, d));
            }
            hit = hit || nearest < 1.0f;
            const glm::vec3 point = center + d * nearest;
            lo = glm::min(lo, point);
            hi = glm::max(hi, point);
        }
        probe.origin = hit ? (lo + hi) * 0.5f : center;
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
