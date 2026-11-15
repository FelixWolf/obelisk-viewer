#include "viewer/render/Frustum.hpp"
#include "viewer/render/Scene.hpp"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using viewer::render::Camera;
using viewer::render::Frustum;

TEST_CASE("Frustum: what a camera sees")
{
    // At the origin looking east (+X), 90 degrees up and down, square.
    Camera camera;
    camera.position = glm::vec3(0.0f);
    camera.target = glm::vec3(1.0f, 0.0f, 0.0f);
    camera.verticalFov = glm::half_pi<float>();
    camera.nearPlane = 0.5f;
    camera.farPlane = 100.0f;
    const Frustum f = Frustum::fromCamera(camera, 1.0f);

    CHECK(f.containsSphere({10.0f, 0.0f, 0.0f}, 0.1f));
    CHECK(f.containsSphere({10.0f, 9.0f, -9.0f}, 0.1f));       // near a corner, inside
    CHECK_FALSE(f.containsSphere({-10.0f, 0.0f, 0.0f}, 1.0f)); // behind
    CHECK_FALSE(f.containsSphere({10.0f, 12.0f, 0.0f}, 1.0f)); // off to the left
    CHECK(f.containsSphere({10.0f, 12.0f, 0.0f}, 2.0f));       // ... but big enough to reach in
    CHECK_FALSE(f.containsSphere({0.2f, 0.0f, 0.0f}, 0.1f));   // before the near plane
    CHECK_FALSE(f.containsSphere({150.0f, 0.0f, 0.0f}, 1.0f)); // past the far plane

    // A region-sized box around the camera is in view; one behind it isn't.
    CHECK(f.containsBox({-128.0f, -128.0f, -10.0f}, {128.0f, 128.0f, 10.0f}));
    CHECK_FALSE(f.containsBox({-300.0f, -128.0f, -10.0f}, {-44.0f, 128.0f, 10.0f}));
    // Wider than tall: a wider aspect sees further sideways.
    const Frustum wide = Frustum::fromCamera(camera, 2.0f);
    CHECK(wide.containsSphere({10.0f, 15.0f, 0.0f}, 0.1f));
    CHECK_FALSE(wide.containsSphere({10.0f, 0.0f, 15.0f}, 0.1f));

    // The default contains everything.
    CHECK(Frustum().containsSphere({1e6f, -1e6f, 0.0f}, 0.0f));

    // Shadows: with the sun straight above, a sphere high over the view
    // (outside it) still shades what's below; one behind the camera doesn't.
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    CHECK_FALSE(f.containsSphere({10.0f, 0.0f, 30.0f}, 1.0f));
    CHECK(f.containsShadowOf({10.0f, 0.0f, 30.0f}, 1.0f, up));
    CHECK_FALSE(f.containsShadowOf({-10.0f, 0.0f, 30.0f}, 1.0f, up));
    // Light from the east: something west of the view casts away from it.
    CHECK_FALSE(f.containsShadowOf({10.0f, 30.0f, 0.0f}, 1.0f, glm::vec3(1.0f, 0.0f, 0.0f)));
}

#include "viewer/render/ShadowCascades.hpp"

TEST_CASE("Shadow cascades: a caster and the ground it shades meet in the map")
{
    using namespace viewer::render;
    Camera camera;
    camera.position = glm::vec3(0.0f, 0.0f, 2.0f);
    camera.target = glm::vec3(1.0f, 0.0f, 1.5f);
    Shadows shadows;
    shadows.enabled = true;
    shadows.towardLight = glm::normalize(glm::vec3(0.3f, 0.6f, 0.8f));
    const Cascades c = computeCascades(camera, 16.0f / 9.0f, shadows, 2048);

    // Splits grow, ending at the shadow distance.
    CHECK(c.splits.x < c.splits.y);
    CHECK(c.splits.z < c.splits.w);
    CHECK(c.splits.w == doctest::Approx(128.0f));

    for(const float distance : {3.0f, 10.0f, 25.0f, 80.0f})
    {
        // The shader's choice of cascade for a ground point that far ahead.
        const glm::vec3 ground(distance, 0.0f, 0.0f);
        const glm::vec3 forward = glm::normalize(camera.target - camera.position);
        const float depth = glm::dot(ground - camera.position, forward);
        const int k = depth < c.splits.x ? 0 : depth < c.splits.y ? 1 : depth < c.splits.z ? 2 : 3;
        CAPTURE(distance);
        CAPTURE(k);

        // Something 5 m up towards the sun shades it.
        const glm::vec3 caster = ground + shadows.towardLight * 5.0f;
        const glm::vec4 g = c.matrices[static_cast<size_t>(k)] * glm::vec4(ground, 1.0f);
        const glm::vec4 s = c.matrices[static_cast<size_t>(k)] * glm::vec4(caster, 1.0f);
        CHECK(g.x == doctest::Approx(s.x).epsilon(1e-3));
        CHECK(g.y == doctest::Approx(s.y).epsilon(1e-3));
        CHECK(s.z < g.z); // nearer the light
        CHECK(g.x >= -1.0f);
        CHECK(g.x <= 1.0f);
        CHECK(g.y >= -1.0f);
        CHECK(g.y <= 1.0f);
        CHECK(g.z >= 0.0f);
        CHECK(g.z <= 1.0f);
        CHECK(s.z >= 0.0f);

        // And the culling keeps it.
        const glm::vec3 inLight(c.lightRotation * glm::vec4(caster, 1.0f));
        CHECK(castsInto(inLight, 0.5f, c.bounds[static_cast<size_t>(k)]));
    }
}

TEST_CASE("Projector: the prim's -Z face is the near plane, fov spans its Y size")
{
    using viewer::render::projectorFor;
    // A unit cube at the origin, unturned, 90 degrees: the apex is at its
    // center (the near plane, its -Z face, is half a meter away and half a
    // meter wide either side).
    const auto p = projectorFor(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f),
                                10.0f, glm::radians(90.0f), 1.0f, 0.5f);
    CHECK(glm::length(p.origin) < 1e-5f);
    CHECK(glm::length(p.axis - glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-5f);
    CHECK(p.depth == doctest::Approx(9.0f)); // range + near - Z size - near
    auto project = [&](glm::vec3 v)
    {
        const glm::vec4 c = p.matrix * glm::vec4(v, 1.0f);
        return glm::vec4(glm::vec3(c) / c.w, c.z);
    };
    const glm::vec4 below = project({0.0f, 0.0f, -2.0f});
    CHECK(below.x == doctest::Approx(0.5f));
    CHECK(below.y == doctest::Approx(0.5f));
    CHECK(below.z > 0.0f);
    CHECK(below.z < 1.0f);
    // The near plane's edge is the texture's edge; +Y is the texture's top
    // (v = 1, rows stored bottom-up).
    CHECK(project({0.5f, 0.0f, -0.5f}).x == doctest::Approx(1.0f));
    CHECK(project({0.0f, 1.0f, -2.0f}).y > 0.5f);
    CHECK(project({0.0f, 0.0f, 1.0f}).w < 0.0f); // behind: not lit

    // Turned to shine along +X, the axis turns with it.
    const auto turned =
        projectorFor(glm::vec3(5.0f, 0.0f, 0.0f),
                     glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                     glm::vec3(1.0f), 10.0f, glm::radians(90.0f), 1.0f, 0.5f);
    CHECK(glm::length(turned.axis - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-5f);
    const glm::vec4 ahead = turned.matrix * glm::vec4(8.0f, 0.0f, 0.0f, 1.0f);
    CHECK(ahead.x / ahead.w == doctest::Approx(0.5f));
}

TEST_CASE("Camera: a headset eye's off-center view")
{
    Camera camera;
    camera.position = glm::vec3(0.0f);
    camera.target = glm::vec3(1.0f, 0.0f, 0.0f);
    camera.verticalFov = glm::half_pi<float>();
    camera.nearPlane = 0.5f;
    camera.farPlane = 100.0f;

    // Without edges, as glm::perspective.
    const glm::mat4 symmetric = camera.projection(1.5f);
    const glm::mat4 expected = glm::perspective(camera.verticalFov, 1.5f, 0.5f, 100.0f);
    for(int c = 0; c < 4; c++)
    {
        for(int r = 0; r < 4; r++)
        {
            CHECK(symmetric[c][r] == doctest::Approx(expected[c][r]).epsilon(1e-5));
        }
    }

    // Off center: further right (-Y) than left, down than up; the aspect
    // ignored.
    camera.edges = glm::vec4(-0.5f, 1.2f, -1.0f, 0.8f); // left, right, down, up
    CHECK(camera.tangents(3.0f) == *camera.edges);
    auto clip = [&](const glm::vec3& p)
    {
        const glm::vec4 c = camera.projection(3.0f) * camera.view() * glm::vec4(p, 1.0f);
        return glm::vec3(c) / c.w;
    };
    // Each edge at x = -1 or 1 (y up) where its tangent says.
    CHECK(clip({10.0f, 5.0f, 0.0f}).x == doctest::Approx(-1.0f));   // left: 0.5 m a meter
    CHECK(clip({10.0f, -12.0f, 0.0f}).x == doctest::Approx(1.0f));  // right: 1.2
    CHECK(clip({10.0f, 0.0f, 8.0f}).y == doctest::Approx(1.0f));    // up: 0.8
    CHECK(clip({10.0f, 0.0f, -10.0f}).y == doctest::Approx(-1.0f)); // down: 1.0
    // Depth 0 at the near plane, 1 at the far.
    CHECK(clip({0.5f, 0.0f, 0.0f}).z == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(clip({100.0f, 0.0f, 0.0f}).z == doctest::Approx(1.0f).epsilon(1e-4));
}
