#include "viewer/render/Frustum.hpp"
#include "viewer/render/Scene.hpp"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

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
    CHECK(f.containsSphere({10.0f, 9.0f, -9.0f}, 0.1f)); // near a corner, inside
    CHECK_FALSE(f.containsSphere({-10.0f, 0.0f, 0.0f}, 1.0f)); // behind
    CHECK_FALSE(f.containsSphere({10.0f, 12.0f, 0.0f}, 1.0f));  // off to the left
    CHECK(f.containsSphere({10.0f, 12.0f, 0.0f}, 2.0f));        // ... but big enough to reach in
    CHECK_FALSE(f.containsSphere({0.2f, 0.0f, 0.0f}, 0.1f));    // before the near plane
    CHECK_FALSE(f.containsSphere({150.0f, 0.0f, 0.0f}, 1.0f));  // past the far plane

    // A region-sized box around the camera is in view; one behind it isn't.
    CHECK(f.containsBox({-128.0f, -128.0f, -10.0f}, {128.0f, 128.0f, 10.0f}));
    CHECK_FALSE(f.containsBox({-300.0f, -128.0f, -10.0f}, {-44.0f, 128.0f, 10.0f}));
    // Wider than tall: a wider aspect sees further sideways.
    const Frustum wide = Frustum::fromCamera(camera, 2.0f);
    CHECK(wide.containsSphere({10.0f, 15.0f, 0.0f}, 0.1f));
    CHECK_FALSE(wide.containsSphere({10.0f, 0.0f, 15.0f}, 0.1f));

    // The default contains everything.
    CHECK(Frustum().containsSphere({1e6f, -1e6f, 0.0f}, 0.0f));
}
