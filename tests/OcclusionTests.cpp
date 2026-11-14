#include "viewer/render/OcclusionMap.hpp"

#include <doctest/doctest.h>

using viewer::render::OcclusionMap;

namespace
{

// With an identity view-projection a point's x, y are where it is on that
// view (-1..1, y up) and its z its depth.
OcclusionMap wall(float depth, uint32_t width = 64, uint32_t height = 32)
{
    return OcclusionMap(width, height, std::vector<float>(width * height, depth), glm::mat4(1.0f),
                        glm::dvec2(256.0, 512.0));
}

} // namespace

TEST_CASE("Occlusion: behind what was drawn is hidden; in front, or off it, is not")
{
    const OcclusionMap map = wall(0.5f);
    CHECK(map.origin() == glm::dvec2(256.0, 512.0));
    CHECK(map.hidden(glm::vec3(0.0f, 0.0f, 0.8f), 0.1f));        // behind the wall
    CHECK_FALSE(map.hidden(glm::vec3(0.0f, 0.0f, 0.3f), 0.1f));  // in front of it
    CHECK_FALSE(map.hidden(glm::vec3(0.0f, 0.0f, 0.55f), 0.1f)); // through it
    CHECK_FALSE(map.hidden(glm::vec3(0.95f, 0.0f, 0.8f), 0.1f)); // partly off that view
    CHECK_FALSE(map.hidden(glm::vec3(3.0f, 0.0f, 0.8f), 0.1f));  // wholly off it
    CHECK_FALSE(map.hidden(glm::vec3(0.0f, 0.0f, 0.05f), 0.1f)); // across the near plane

    // Nothing drawn (the far plane): nothing hidden.
    CHECK_FALSE(wall(1.0f).hidden(glm::vec3(0.0f, 0.0f, 0.8f), 0.1f));
    // An empty map hides nothing.
    CHECK_FALSE(OcclusionMap(0, 0, {}, glm::mat4(1.0f), {}).hidden(glm::vec3(0.0f), 0.1f));
}

TEST_CASE("Occlusion: a hole in the wall shows what's behind it, at any size")
{
    const uint32_t width = 64;
    const uint32_t height = 32;
    std::vector<float> depth(width * height, 0.5f);
    // A hole at texel (48, 8): x 0.5..0.53, y 0.5..0.44 in view coordinates.
    depth[8 * width + 48] = 1.0f;
    const OcclusionMap map(width, height, std::move(depth), glm::mat4(1.0f), {});
    // Small and big boxes over the hole see through it; elsewhere they don't.
    for(const float r : {0.01f, 0.1f, 0.3f})
    {
        CHECK_FALSE(map.hidden(glm::vec3(0.52f, 0.47f, 0.8f), glm::vec3(r, r, 0.05f)));
    }
    CHECK(map.hidden(glm::vec3(-0.5f, -0.5f, 0.8f), glm::vec3(0.1f, 0.1f, 0.05f)));
}
