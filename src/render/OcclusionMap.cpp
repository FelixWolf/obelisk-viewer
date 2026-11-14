#include "viewer/render/OcclusionMap.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::render
{

namespace
{

// Rectangles at most this many texels across are tested at a level where
// they cover no more than 2 or 3 texels each way.
constexpr float kTexelsAcross = 2.0f;

} // namespace

OcclusionMap::OcclusionMap(uint32_t width, uint32_t height, std::vector<float> depth,
                           const glm::mat4& viewProjection, glm::dvec2 origin)
    : viewProjection_(viewProjection), origin_(origin)
{
    if(width == 0 || height == 0 || depth.size() < static_cast<size_t>(width) * height)
    {
        return;
    }
    levels_.push_back({width, height, std::move(depth)});
    while(levels_.back().width > 1 || levels_.back().height > 1)
    {
        const Level& fine = levels_.back();
        Level coarse;
        coarse.width = (fine.width + 1) / 2;
        coarse.height = (fine.height + 1) / 2;
        coarse.depth.resize(static_cast<size_t>(coarse.width) * coarse.height);
        for(uint32_t y = 0; y < coarse.height; y++)
        {
            for(uint32_t x = 0; x < coarse.width; x++)
            {
                float farthest = 0.0f;
                for(uint32_t dy = 0; dy < 2; dy++)
                {
                    for(uint32_t dx = 0; dx < 2; dx++)
                    {
                        const uint32_t fx = std::min(x * 2 + dx, fine.width - 1);
                        const uint32_t fy = std::min(y * 2 + dy, fine.height - 1);
                        farthest = std::max(farthest, fine.depth[fy * fine.width + fx]);
                    }
                }
                coarse.depth[y * coarse.width + x] = farthest;
            }
        }
        levels_.push_back(std::move(coarse));
    }
}

bool OcclusionMap::hidden(const glm::vec3& center, const glm::vec3& halfSize) const
{
    if(levels_.empty())
    {
        return false;
    }
    // Its box's corners in that view.
    glm::vec2 lo(1e9f);
    glm::vec2 hi(-1e9f);
    float nearest = 1e9f;
    for(int i = 0; i < 8; i++)
    {
        const glm::vec3 corner =
            center + halfSize * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                                          (i & 4) ? 1.0f : -1.0f);
        const glm::vec4 clip = viewProjection_ * glm::vec4(corner, 1.0f);
        if(clip.w <= 1e-4f)
        {
            return false; // across that camera's plane
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        lo = glm::min(lo, glm::vec2(ndc));
        hi = glm::max(hi, glm::vec2(ndc));
        nearest = std::min(nearest, ndc.z);
    }
    // Wholly inside that view, in front of its near plane; otherwise it
    // can't tell.
    if(lo.x < -1.0f || lo.y < -1.0f || hi.x > 1.0f || hi.y > 1.0f || nearest <= 0.0f)
    {
        return false;
    }

    // Its rectangle in texels (rows from the top), at a level where it's a
    // few texels across.
    const Level& base = levels_[0];
    const glm::vec2 size(static_cast<float>(base.width), static_cast<float>(base.height));
    const glm::vec2 a = glm::vec2(lo.x * 0.5f + 0.5f, 0.5f - hi.y * 0.5f) * size;
    const glm::vec2 b = glm::vec2(hi.x * 0.5f + 0.5f, 0.5f - lo.y * 0.5f) * size;
    const float across = std::max(b.x - a.x, b.y - a.y);
    size_t level = 0;
    while(level + 1 < levels_.size() && across / static_cast<float>(1u << level) > kTexelsAcross)
    {
        level++;
    }
    const Level& l = levels_[level];
    const float scale = 1.0f / static_cast<float>(1u << level);
    const auto x0 = static_cast<uint32_t>(
        std::clamp(std::floor(a.x * scale), 0.0f, static_cast<float>(l.width - 1)));
    const auto x1 = static_cast<uint32_t>(
        std::clamp(std::floor(b.x * scale), 0.0f, static_cast<float>(l.width - 1)));
    const auto y0 = static_cast<uint32_t>(
        std::clamp(std::floor(a.y * scale), 0.0f, static_cast<float>(l.height - 1)));
    const auto y1 = static_cast<uint32_t>(
        std::clamp(std::floor(b.y * scale), 0.0f, static_cast<float>(l.height - 1)));
    for(uint32_t y = y0; y <= y1; y++)
    {
        for(uint32_t x = x0; x <= x1; x++)
        {
            if(l.depth[y * l.width + x] >= nearest)
            {
                return false; // something there is no nearer than it
            }
        }
    }
    return true;
}

} // namespace viewer::render
