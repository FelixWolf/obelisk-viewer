#pragma once

// Clustered lights and reflection probes: the view split into tiles across
// and slices in depth (each slice deeper than the last, as far is coarser),
// and for each cluster the local lights and probes that can reach it, so a
// surface shader looks only at those (lights.glsl, probes.glsl) instead of
// every one in use.
//
// The data is one array of uints: a header of four per cluster (where its
// lights start in the array, how many; where its probes start, how many),
// then the index lists. Clusters are numbered across, then down (tile rows
// from the top, as the fragment coordinate counts), then slice by slice.

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace viewer::render
{

struct ClusterGrid
{
    uint32_t tilesX = 16;
    uint32_t tilesY = 16;
    uint32_t slices = 24;
    float near = 0.25f; // meters along the view: the first slice starts here
    float far = 512.0f; // the last ends here (and holds anything further)
    uint32_t width = 0; // the view, pixels
    uint32_t height = 0;

    uint32_t clusters() const { return tilesX * tilesY * slices; }
    // The grid for a view `width` x `height`: 16 tiles across, as many down
    // as keeps them square-ish.
    static ClusterGrid forView(uint32_t width, uint32_t height, float near, float far);
};

// Something that lights or reflects within a sphere (scene coordinates).
struct ClusterSphere
{
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// The clusters' lists for a view (`view` and `projection` its camera's;
// the projection perspective). `lights` and `probes` in the order the
// shaders have them: their indices go in the lists.
std::vector<uint32_t> buildClusters(const ClusterGrid& grid, const glm::mat4& view,
                                    const glm::mat4& projection,
                                    std::span<const ClusterSphere> lights,
                                    std::span<const ClusterSphere> probes);

} // namespace viewer::render
