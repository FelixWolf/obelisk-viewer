#include "AvatarLoading.hpp"

#include <doctest/doctest.h>

using viewer::AvatarLoading;
using viewer::Uuid;

namespace
{

AvatarLoading::Inputs ready()
{
    AvatarLoading::Inputs in;
    in.missingParts = false;
    return in;
}

} // namespace

TEST_CASE("AvatarLoading: a cloud until everything has settled, then never again")
{
    AvatarLoading a;
    AvatarLoading::Inputs in;
    CHECK_FALSE(a.update(in, 10.0));
    CHECK_FALSE(a.update(in, 500.0)); // no appearance: for as long as it takes
    in = ready();
    CHECK_FALSE(a.update(in, 501.0));
    CHECK_FALSE(a.update(in, 502.9));
    in.meshesPending = true; // another mesh: the wait starts over
    CHECK_FALSE(a.update(in, 502.95));
    in.meshesPending = false;
    CHECK_FALSE(a.update(in, 505.0));
    CHECK(a.update(in, 506.0));
    CHECK(a.update(AvatarLoading::Inputs{}, 507.0));
}

TEST_CASE("AvatarLoading: bakes are waited for a minute, from the latest ones")
{
    AvatarLoading a;
    auto in = ready();
    in.bakes = 1;
    in.bakesPending = true;
    CHECK_FALSE(a.update(in, 0.0));
    CHECK_FALSE(a.update(in, 59.0));
    in.bakes = 2;
    CHECK_FALSE(a.update(in, 59.5));
    CHECK_FALSE(a.update(in, 119.0));
    CHECK_FALSE(a.update(in, 120.0));
    CHECK(a.update(in, 122.5));
}

TEST_CASE("AvatarLoading: attachments the simulator lists are waited for")
{
    AvatarLoading a;
    auto in = ready();
    in.expected = 2;
    CHECK_FALSE(a.update(in, 0.0));
    in.attachments = 1;
    CHECK_FALSE(a.update(in, 30.0));
    CHECK_FALSE(a.update(in, 89.0)); // under a minute since the last came
    SUBCASE("all here")
    {
        in.attachments = 2;
        CHECK_FALSE(a.update(in, 90.0));
        CHECK(a.update(in, 92.5));
    }
    SUBCASE("one never comes")
    {
        CHECK_FALSE(a.update(in, 91.0));
        CHECK(a.update(in, 92.5));
    }
    SUBCASE("more listed")
    {
        in.attachments = 2;
        CHECK_FALSE(a.update(in, 90.0));
        in.expected = 3;
        CHECK_FALSE(a.update(in, 91.0));
        CHECK_FALSE(a.update(in, 100.0));
    }
}

TEST_CASE("AvatarLoading: missing parts are a shape or a main bake")
{
    viewer::world::AvatarAppearance look;
    CHECK(viewer::hasMissingParts(nullptr));
    look.visualParams = {1, 2, 3};
    CHECK(viewer::hasMissingParts(&look));
    for(const size_t slot : {8, 9, 10})
    {
        look.faces[slot].texture =
            *Uuid::parse("3a367d1c-bef1-6d43-7595-e88c1e3aadb3"); // invisible
    }
    CHECK_FALSE(viewer::hasMissingParts(&look));
    look.faces[9].texture = *Uuid::parse("c228d1cf-4b5d-4ba8-84f4-899a0796aa97");
    CHECK(viewer::hasMissingParts(&look));
    look.faces[9].texture = look.faces[8].texture;
    look.visualParams.clear();
    CHECK(viewer::hasMissingParts(&look));
}
