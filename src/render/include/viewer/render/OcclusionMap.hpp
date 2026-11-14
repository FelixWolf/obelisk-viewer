#pragma once

// Occlusion culling (what the official viewer does with occlusion queries,
// which SDL GPU hasn't): a small copy of a past frame's depth, each texel
// the farthest depth in its block of the view, with the view-projection it
// was drawn with. Something is hidden if, wherever it would have covered
// that view, its nearest point is behind the farthest thing drawn there.
// The copy is a frame or two old (read back without waiting), so the test
// is conservative: what it can't be sure of (off that view's edges, across
// its camera plane) counts as seen.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace viewer::render
{

class OcclusionMap
{
public:
    // `depth`: width x height, row by row from the top, depth 0..1 (1 the
    // far plane: nothing). `origin`: the scene's origin it was drawn at
    // (Scene::worldOrigin).
    OcclusionMap(uint32_t width, uint32_t height, std::vector<float> depth,
                 const glm::mat4& viewProjection, glm::dvec2 origin);

    // Whether a box (scene coordinates, its center and half its size) is
    // certainly hidden.
    bool hidden(const glm::vec3& center, const glm::vec3& halfSize) const;
    bool hidden(const glm::vec3& center, float radius) const
    {
        return hidden(center, glm::vec3(radius));
    }

    glm::dvec2 origin() const { return origin_; }
    uint32_t width() const { return levels_.empty() ? 0 : levels_[0].width; }
    uint32_t height() const { return levels_.empty() ? 0 : levels_[0].height; }

private:
    // A pyramid: each level the farthest of 2x2 of the one before.
    struct Level
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> depth;
    };
    std::vector<Level> levels_;
    glm::mat4 viewProjection_{1.0f};
    glm::dvec2 origin_{0.0};
};

} // namespace viewer::render
