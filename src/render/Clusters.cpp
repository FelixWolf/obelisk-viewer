#include "viewer/render/Clusters.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::render
{

ClusterGrid ClusterGrid::forView(uint32_t width, uint32_t height, float near, float far)
{
    ClusterGrid g;
    g.width = std::max(width, 1u);
    g.height = std::max(height, 1u);
    g.tilesX = 16;
    g.tilesY = std::clamp(static_cast<uint32_t>(std::lround(16.0 * g.height / g.width)), 1u, 64u);
    g.near = std::max(near, 0.05f);
    g.far = std::max(far, g.near * 2.0f);
    return g;
}

namespace
{

// The clusters a sphere may reach: tile and slice ranges (inclusive);
// false if none.
struct Range
{
    uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0, s0 = 0, s1 = 0;
};

bool rangeOf(const ClusterGrid& g, const glm::mat4& view, const glm::mat4& projection,
             const ClusterSphere& sphere, Range& out)
{
    const glm::vec3 c(view * glm::vec4(sphere.center, 1.0f));
    const float r = sphere.radius;
    const float depth = -c.z; // along the view
    if(depth + r < g.near)
    {
        return false; // wholly behind the first slice
    }
    // Slices: by depth, each a constant factor deeper.
    const float logRange = std::log(g.far / g.near);
    auto sliceOf = [&](float d)
    {
        const float s =
            std::log(std::max(d, g.near) / g.near) / logRange * static_cast<float>(g.slices);
        return static_cast<uint32_t>(std::clamp(s, 0.0f, static_cast<float>(g.slices - 1)));
    };
    out.s0 = sliceOf(depth - r);
    out.s1 = sliceOf(depth + r);

    // Tiles: its box's corners on screen, or all of them if it reaches the
    // near plane (the projection breaks down there).
    out.x0 = 0;
    out.x1 = g.tilesX - 1;
    out.y0 = 0;
    out.y1 = g.tilesY - 1;
    if(depth - r > g.near * 0.5f)
    {
        glm::vec2 lo(1e9f);
        glm::vec2 hi(-1e9f);
        for(int i = 0; i < 8; i++)
        {
            const glm::vec3 corner =
                c + r * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                                  (i & 4) ? 1.0f : -1.0f);
            const glm::vec4 clip = projection * glm::vec4(corner, 1.0f);
            const glm::vec2 ndc = glm::vec2(clip) / std::max(clip.w, 1e-6f);
            lo = glm::min(lo, ndc);
            hi = glm::max(hi, ndc);
        }
        if(hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f)
        {
            return false; // off the view
        }
        auto tile = [](float t, uint32_t count)
        {
            return static_cast<uint32_t>(std::clamp(std::floor(t * static_cast<float>(count)), 0.0f,
                                                    static_cast<float>(count - 1)));
        };
        // x from the left; y from the top (clip space y is up).
        out.x0 = tile(lo.x * 0.5f + 0.5f, g.tilesX);
        out.x1 = tile(hi.x * 0.5f + 0.5f, g.tilesX);
        out.y0 = tile(0.5f - hi.y * 0.5f, g.tilesY);
        out.y1 = tile(0.5f - lo.y * 0.5f, g.tilesY);
    }
    return true;
}

} // namespace

std::vector<uint32_t> buildClusters(const ClusterGrid& g, const glm::mat4& view,
                                    const glm::mat4& projection,
                                    std::span<const ClusterSphere> lights,
                                    std::span<const ClusterSphere> probes)
{
    const uint32_t clusters = g.clusters();
    // Which (cluster, index) pairs, for lights then probes; then counted
    // and laid out cluster by cluster.
    std::vector<std::pair<uint32_t, uint32_t>> pairs[2];
    for(int kind = 0; kind < 2; kind++)
    {
        const auto spheres = kind == 0 ? lights : probes;
        for(uint32_t i = 0; i < spheres.size(); i++)
        {
            Range r;
            if(!rangeOf(g, view, projection, spheres[i], r))
            {
                continue;
            }
            for(uint32_t s = r.s0; s <= r.s1; s++)
            {
                for(uint32_t y = r.y0; y <= r.y1; y++)
                {
                    for(uint32_t x = r.x0; x <= r.x1; x++)
                    {
                        pairs[kind].emplace_back((s * g.tilesY + y) * g.tilesX + x, i);
                    }
                }
            }
        }
    }
    std::vector<uint32_t> data(
        static_cast<size_t>(clusters) * 4 + pairs[0].size() + pairs[1].size(), 0);
    uint32_t at = clusters * 4;
    for(int kind = 0; kind < 2; kind++)
    {
        // Counts, then each cluster's start, then the indices in place.
        for(const auto& [cluster, index] : pairs[kind])
        {
            data[cluster * 4 + kind * 2 + 1]++;
        }
        for(uint32_t c = 0; c < clusters; c++)
        {
            data[c * 4 + kind * 2] = at;
            at += data[c * 4 + kind * 2 + 1];
            data[c * 4 + kind * 2 + 1] = 0; // refilled below
        }
        for(const auto& [cluster, index] : pairs[kind])
        {
            uint32_t& count = data[cluster * 4 + kind * 2 + 1];
            data[data[cluster * 4 + kind * 2] + count] = index;
            count++;
        }
    }
    return data;
}

} // namespace viewer::render
