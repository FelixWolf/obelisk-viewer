#include "viewer/volume/Volume.hpp"

#include <doctest/doctest.h>

using namespace viewer::volume;

namespace
{

VolumeParams shape(uint8_t profile, uint8_t path)
{
    VolumeParams p;
    p.profile.curve = profile;
    p.path.curve = path;
    return p;
}

// The in-world prim types, as the build tools create them.
VolumeParams box()
{
    return shape(ProfileCurve::Square, PathCurve::Line);
}
VolumeParams cylinder()
{
    return shape(ProfileCurve::Circle, PathCurve::Line);
}
VolumeParams prism()
{
    return shape(ProfileCurve::EqualTriangle, PathCurve::Line);
}
VolumeParams sphere()
{
    return shape(ProfileCurve::HalfCircle, PathCurve::Circle);
}
VolumeParams torus()
{
    VolumeParams p = shape(ProfileCurve::Circle, PathCurve::Circle);
    p.path.scale = {1.0f, 0.25f};
    return p;
}
VolumeParams tube()
{
    VolumeParams p = shape(ProfileCurve::Square, PathCurve::Circle);
    p.path.scale = {1.0f, 0.25f};
    return p;
}
VolumeParams ring()
{
    VolumeParams p = shape(ProfileCurve::EqualTriangle, PathCurve::Circle);
    p.path.scale = {1.0f, 0.25f};
    return p;
}

void checkFaces(const std::vector<VolumeFace>& faces)
{
    for(const VolumeFace& f : faces)
    {
        REQUIRE(f.positions.size() == f.normals.size());
        REQUIRE(f.positions.size() == f.texCoords.size());
        REQUIRE(f.indices.size() % 3 == 0);
        CHECK_FALSE(f.indices.empty());
        for(const uint32_t i : f.indices)
        {
            REQUIRE(i < f.positions.size());
        }
        for(const glm::vec3& p : f.positions)
        {
            CHECK(std::abs(p.x) <= 0.5001f);
            CHECK(std::abs(p.y) <= 0.5001f);
            CHECK(std::abs(p.z) <= 0.5001f);
        }
    }
}

} // namespace

TEST_CASE("Volume: face counts match the in-world prim types")
{
    const float detail = kDetailScales[3];
    CHECK(generateVolume(box(), detail).size() == 6);
    CHECK(generateVolume(cylinder(), detail).size() == 3);
    CHECK(generateVolume(prism(), detail).size() == 5);
    CHECK(generateVolume(sphere(), detail).size() == 1);
    CHECK(generateVolume(torus(), detail).size() == 1);
    CHECK(generateVolume(tube(), detail).size() == 4);
    CHECK(generateVolume(ring(), detail).size() == 3);

    VolumeParams hollowBox = box();
    hollowBox.profile.hollow = 0.5f;
    CHECK(generateVolume(hollowBox, detail).size() == 7);

    VolumeParams cutBox = box();
    cutBox.profile.begin = 0.125f;
    cutBox.profile.end = 0.875f;
    // 2 caps, the 4 (partial) sides, and the 2 faces the cut exposes.
    CHECK(generateVolume(cutBox, detail).size() == 8);

    for(const VolumeParams& p :
        {box(), cylinder(), prism(), sphere(), torus(), tube(), ring(), hollowBox, cutBox})
    {
        checkFaces(generateVolume(p, detail));
    }
}

TEST_CASE("Volume: a box is the unit cube with outward normals")
{
    const auto faces = generateVolume(box(), kDetailScales[3]);
    REQUIRE(faces.size() == 6);
    // Face 0 is the top, 1-4 the sides, 5 the bottom (as in-world).
    CHECK((faces[0].type & FaceType::Top) != 0);
    CHECK((faces[5].type & FaceType::Bottom) != 0);
    for(const glm::vec3& n : faces[0].normals)
    {
        CHECK(n.z == doctest::Approx(1.0f));
    }
    for(const glm::vec3& n : faces[5].normals)
    {
        CHECK(n.z == doctest::Approx(-1.0f));
    }
    for(size_t f = 0; f < faces.size(); f++)
    {
        for(size_t i = 0; i < faces[f].positions.size(); i++)
        {
            // Every vertex lies on the face its normal points out of.
            CHECK(glm::dot(faces[f].positions[i], faces[f].normals[i]) == doctest::Approx(0.5f));
        }
        // Triangles wind counter-clockwise seen from outside.
        const auto& face = faces[f];
        for(size_t t = 0; t < face.indices.size(); t += 3)
        {
            const glm::vec3 a = face.positions[face.indices[t]];
            const glm::vec3 b = face.positions[face.indices[t + 1]];
            const glm::vec3 c = face.positions[face.indices[t + 2]];
            const glm::vec3 n = glm::cross(b - a, c - a);
            if(glm::length(n) > 1e-6f)
            {
                CHECK(glm::dot(n, face.normals[face.indices[t]]) > 0.0f);
            }
        }
    }
}

TEST_CASE("Volume: cylinders and spheres are round")
{
    for(const glm::vec3& p : generateVolume(cylinder(), kDetailScales[3])[1].positions)
    {
        CHECK(glm::length(glm::vec2(p.x, p.y)) == doctest::Approx(0.5f).epsilon(0.001));
    }
    for(const glm::vec3& p : generateVolume(sphere(), kDetailScales[3])[0].positions)
    {
        CHECK(glm::length(p) == doctest::Approx(0.5f).epsilon(0.001));
    }
}

TEST_CASE("Volume: unpacking quantized parameters")
{
    PackedVolumeParams packed;
    packed.pathCurve = PathCurve::Line;
    packed.profileCurve = ProfileCurve::Square;
    packed.pathScaleX = 100; // 200 - 100 = 1.0
    packed.pathScaleY = 100;
    packed.pathRevolutions = 0;
    packed.profileHollow = 25000; // 0.5
    packed.profileBegin = 6250;   // 0.125
    packed.pathTwist = -50;       // -0.5
    const VolumeParams v = unpackVolumeParams(packed);
    CHECK(v.path.scale.x == doctest::Approx(1.0f));
    CHECK(v.path.end == doctest::Approx(1.0f));
    CHECK(v.path.revolutions == doctest::Approx(1.0f));
    CHECK(v.profile.hollow == doctest::Approx(0.5f));
    CHECK(v.profile.begin == doctest::Approx(0.125f));
    CHECK(v.profile.end == doctest::Approx(1.0f));
    CHECK(v.path.twist == doctest::Approx(-0.5f));
}
