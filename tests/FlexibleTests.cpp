#include "FlexibleView.hpp"

#include <doctest/doctest.h>

using viewer::FlexibleView;

TEST_CASE("Flexible prims: sections remapped straight, skipped and interpolated")
{
    FlexibleView::Sections source{};
    source[0].position = glm::vec3(1.0f, 2.0f, 3.0f);
    source[0].direction = glm::vec3(0.0f, 0.0f, 1.0f);
    source[0].dPosition = source[0].direction;

    // From section 0 alone: four sections straight up its direction.
    FlexibleView::Sections straight{};
    FlexibleView::remap(source, -1, straight, 2, 2.0f);
    for(int i = 0; i <= 4; i++)
    {
        CHECK(straight[i].position.z == doctest::Approx(3.0f + 0.5f * static_cast<float>(i)));
        CHECK(straight[i].position.x == doctest::Approx(1.0f));
    }

    // Fewer: every other one.
    FlexibleView::Sections fewer{};
    FlexibleView::remap(straight, 2, fewer, 1, 2.0f);
    CHECK(fewer[1].position.z == doctest::Approx(4.0f));
    CHECK(fewer[2].position.z == doctest::Approx(5.0f));

    // More: a straight chain stays straight, evenly spaced.
    for(auto& s : fewer)
    {
        s.dPosition = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    FlexibleView::Sections more{};
    FlexibleView::remap(fewer, 1, more, 3, 2.0f);
    for(int i = 0; i <= 8; i++)
    {
        CHECK(more[i].position.z == doctest::Approx(3.0f + 0.25f * static_cast<float>(i)));
        CHECK(more[i].position.x == doctest::Approx(1.0f));
    }
}

TEST_CASE("Flexible: small ones on screen are updated less often, at most 32 flexi frames apart")
{
    CHECK(FlexibleView::updatePeriod(1.0f) == 1);   // filling the view
    CHECK(FlexibleView::updatePeriod(0.01f) == 1);  // a hundredth of it
    CHECK(FlexibleView::updatePeriod(0.002f) == 3); // 0.005 / 0.002 + 1
    CHECK(FlexibleView::updatePeriod(0.0f) == 32);
    CHECK(FlexibleView::updatePeriod(1e-5f) == 32);
}
