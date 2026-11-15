#include "viewer/render/Clusters.hpp"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using viewer::render::buildClusters;
using viewer::render::ClusterGrid;
using viewer::render::ClusterSphere;

namespace
{

// A small repeatable random number generator, 0..1.
struct Random
{
    uint32_t state = 12345;
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
    float between(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// The cluster a point at pixel (px, py) (from the top left) and `depth`
// along the view falls in, as clusters.glsl finds it.
uint32_t clusterAt(const ClusterGrid& g, float px, float py, float depth)
{
    const float tileWidth = static_cast<float>(g.width) / static_cast<float>(g.tilesX);
    const float tileHeight = static_cast<float>(g.height) / static_cast<float>(g.tilesY);
    const auto x = std::clamp(static_cast<int>(px / tileWidth), 0, static_cast<int>(g.tilesX) - 1);
    const auto y = std::clamp(static_cast<int>(py / tileHeight), 0, static_cast<int>(g.tilesY) - 1);
    const float logRange = std::log(g.far / g.near);
    const auto slice = std::clamp(static_cast<int>(std::log(std::max(depth, g.near) / g.near) /
                                                   logRange * static_cast<float>(g.slices)),
                                  0, static_cast<int>(g.slices) - 1);
    return (static_cast<uint32_t>(slice) * g.tilesY + static_cast<uint32_t>(y)) * g.tilesX +
           static_cast<uint32_t>(x);
}

bool listed(const std::vector<uint32_t>& data, uint32_t cluster, int kind, uint32_t index)
{
    const uint32_t start = data[cluster * 4 + kind * 2];
    const uint32_t count = data[cluster * 4 + kind * 2 + 1];
    return std::find(data.begin() + start, data.begin() + start + count, index) !=
           data.begin() + start + count;
}

} // namespace

TEST_CASE("Clusters: every light and probe reaching a point is in that point's cluster")
{
    const uint32_t width = 1600;
    const uint32_t height = 900;
    const ClusterGrid grid = ClusterGrid::forView(width, height, 0.1f, 512.0f);
    CHECK(grid.tilesX == 16);
    CHECK(grid.tilesY == 9);

    // Looking along -z from just off the origin, turned a little.
    const glm::vec3 eye(3.0f, -2.0f, 1.5f);
    const glm::vec3 forward = glm::normalize(glm::vec3(0.3f, 0.1f, -1.0f));
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(
        glm::radians(60.0f), static_cast<float>(width) / static_cast<float>(height), 0.1f, 1024.0f);
    const glm::mat4 inverse = glm::inverse(projection * view);

    Random random;
    std::vector<ClusterSphere> lights;
    std::vector<ClusterSphere> probes;
    for(int i = 0; i < 200; i++)
    {
        // Mostly in front, some around and behind the camera, some
        // reaching through the near plane; a few big.
        const glm::vec3 at =
            eye + glm::vec3(random.between(-60.0f, 60.0f), random.between(-30.0f, 30.0f),
                            random.between(-120.0f, 10.0f));
        const float radius =
            i % 20 == 0 ? random.between(20.0f, 80.0f) : random.between(0.2f, 12.0f);
        (i % 3 == 0 ? probes : lights).push_back({at, radius});
    }
    lights.push_back({eye + forward * 0.05f, 0.5f}); // around the camera
    const std::vector<uint32_t> data = buildClusters(grid, view, projection, lights, probes);
    REQUIRE(data.size() >= grid.clusters() * 4);

    // Points across the view at all depths (pixel centers, as fragments).
    int reached = 0;
    for(int sample = 0; sample < 200000; sample++)
    {
        const float px = std::floor(random.next() * static_cast<float>(width)) + 0.5f;
        const float py = std::floor(random.next() * static_cast<float>(height)) + 0.5f;
        const float depth = std::exp(random.between(std::log(0.1f), std::log(200.0f)));
        // Through that pixel (y from the top), `depth` along the view.
        const glm::vec4 far =
            inverse * glm::vec4(px / static_cast<float>(width) * 2.0f - 1.0f,
                                1.0f - py / static_cast<float>(height) * 2.0f, 1.0f, 1.0f);
        const glm::vec3 direction = glm::normalize(glm::vec3(far) / far.w - eye);
        const glm::vec3 point = eye + direction * (depth / glm::dot(direction, forward));
        const uint32_t cluster = clusterAt(grid, px, py, depth);
        for(int kind = 0; kind < 2; kind++)
        {
            const auto& spheres = kind == 0 ? lights : probes;
            for(uint32_t i = 0; i < spheres.size(); i++)
            {
                if(glm::length(point - spheres[i].center) < spheres[i].radius)
                {
                    reached++;
                    if(!listed(data, cluster, kind, i))
                    {
                        FAIL_CHECK("kind " << kind << " index " << i << " missing at pixel " << px
                                           << "," << py << " depth " << depth);
                    }
                }
            }
        }
    }
    CHECK(reached > 1000);
}

TEST_CASE("Clusters: a light behind the camera or off the view is in no cluster")
{
    const ClusterGrid grid = ClusterGrid::forView(800, 800, 0.1f, 512.0f);
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1024.0f);
    const std::vector<ClusterSphere> lights = {
        {glm::vec3(0.0f, 0.0f, 10.0f), 2.0f},    // behind (the view looks along -z)
        {glm::vec3(100.0f, 0.0f, -10.0f), 2.0f}, // far off to the side
        {glm::vec3(0.0f, 0.0f, -10.0f), 1.0f},   // straight ahead
    };
    const std::vector<uint32_t> data = buildClusters(grid, glm::mat4(1.0f), projection, lights, {});
    std::vector<uint32_t> seen(lights.size(), 0);
    for(uint32_t c = 0; c < grid.clusters(); c++)
    {
        for(uint32_t k = 0; k < data[c * 4 + 1]; k++)
        {
            seen[data[data[c * 4] + k]]++;
        }
        CHECK(data[c * 4 + 3] == 0); // no probes
    }
    CHECK(seen[0] == 0);
    CHECK(seen[1] == 0);
    CHECK(seen[2] > 0);
    CHECK(seen[2] < grid.clusters() / 16); // only near the middle
    CHECK(data.size() == grid.clusters() * 4 + seen[2]);
}
