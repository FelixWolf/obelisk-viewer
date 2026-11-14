// Prim geometry. Behavior checked against the official viewer's llvolume.cpp
// (LLProfile::genNGon/addHole/generate, LLPath::genNGon/generate,
// LLVolume::generate/createVolumeFaces, LLVolumeFace::createSide/
// createCap/createUnCutCubeCap); our own implementation.

#include "viewer/volume/Volume.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace viewer::volume
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMinDetailFaces = 6;

// Profile face ids (which part of the prim a face is).
namespace FaceId
{
constexpr uint16_t PathBegin = 1 << 0;
constexpr uint16_t PathEnd = 1 << 1;
constexpr uint16_t InnerSide = 1 << 2;
constexpr uint16_t ProfileBegin = 1 << 3;
constexpr uint16_t ProfileEnd = 1 << 4;
constexpr uint16_t OuterSide0 = 1 << 5;
} // namespace FaceId

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// --- Profile (the cross-section) ------------------------------------------------

struct ProfileFace
{
    int index = 0;
    int count = 0;
    float scaleU = 0.0f;
    bool cap = false;
    bool flat = false;
    uint16_t id = 0;
};

// Points are (x, y, u): u runs along the outline and becomes the side
// faces' texture coordinate.
struct Profile
{
    std::vector<glm::vec3> points;
    std::vector<ProfileFace> faces;
    bool open = false;
    bool concave = false;
    int total = 0;
    int totalOut = 0;

    void addCap(uint16_t id) { faces.push_back({0, total, 1.0f, true, false, id}); }

    void addFace(int index, int count, float scaleU, uint16_t id, bool flat)
    {
        faces.push_back({index, count, scaleU, false, flat, id});
    }

    // A regular polygon's arc from params.begin to params.end.
    void genNGon(const ProfileParams& params, int sides, float offset, float angScale, int split)
    {
        constexpr float kTableScale[] = {1, 1, 1, 0.5f, 0.707107f, 0.53f, 0.525f, 0.5f};
        float scale = 0.5f;
        const float begin = params.begin;
        const float end = params.end;
        const float tStep = 1.0f / static_cast<float>(sides);
        const float angStep = 2.0f * kPi * tStep * angScale;

        const int totalSides = static_cast<int>(std::lround(static_cast<float>(sides) / angScale));
        if(totalSides < 8)
        {
            scale = kTableScale[totalSides];
        }

        const float tFirst =
            std::floor(begin * static_cast<float>(sides)) / static_cast<float>(sides);
        float t = tFirst;
        float ang = 2.0f * kPi * (t * angScale + offset);
        glm::vec3 pt1(std::cos(ang) * scale, std::sin(ang) * scale, t);
        t += tStep;
        ang += angStep;
        glm::vec3 pt2(std::cos(ang) * scale, std::sin(ang) * scale, t);
        float fraction = (begin - tFirst) * static_cast<float>(sides);

        // The partial first edge.
        if(fraction < 0.9999f)
        {
            points.push_back(glm::mix(pt1, pt2, fraction));
        }

        auto pushSplit = [&](const glm::vec3& next)
        {
            if(!points.empty())
            {
                const glm::vec3 p = points.back();
                for(int i = 0; i < split; i++)
                {
                    points.push_back(p + (next - p) * (1.0f / static_cast<float>(split + 1) *
                                                       static_cast<float>(i + 1)));
                }
            }
        };

        // Whole edges.
        while(t < end)
        {
            pt1 = glm::vec3(std::cos(ang) * scale, std::sin(ang) * scale, t);
            pushSplit(pt1);
            points.push_back(pt1);
            t += tStep;
            ang += angStep;
        }

        // The partial last edge.
        fraction = (end - (t - tStep)) * static_cast<float>(sides);
        pt2 = glm::vec3(std::cos(ang) * scale, std::sin(ang) * scale, t);
        if(fraction > 0.0001f)
        {
            const glm::vec3 last = glm::mix(pt1, pt2, fraction);
            pushSplit(last);
            points.push_back(last);
        }

        // Cut: add the center (unless hollow).
        if((end - begin) * angScale < 0.99f)
        {
            concave = (end - begin) * angScale > 0.5f;
            open = true;
            if(params.hollow <= 0.0f)
            {
                points.push_back(glm::vec3(0.0f));
            }
        }
        else
        {
            open = false;
            concave = false;
        }
        total = static_cast<int>(points.size());
    }

    // The hollow: a smaller polygon, scaled and walked in reverse.
    void addHole(const ProfileParams& params, bool flat, float sides, float offset, float hollow,
                 float angScale, int split = 0)
    {
        totalOut = total;
        genNGon(params, static_cast<int>(std::floor(sides)), offset, angScale, split);
        addFace(totalOut, total - totalOut, 0.0f, FaceId::InnerSide, flat);

        std::vector<glm::vec3> hole(points.begin() + totalOut, points.begin() + total);
        for(glm::vec3& p : hole)
        {
            p *= hollow;
        }
        int j = static_cast<int>(hole.size()) - 1;
        for(int i = totalOut; i < total; i++)
        {
            points[static_cast<size_t>(i)] = hole[static_cast<size_t>(j--)];
        }
        for(ProfileFace& face : faces)
        {
            if(face.cap)
            {
                face.count *= 2;
            }
        }
    }

    // sculptSize > 0: a sculpted prim's profile, a circle of that many sides.
    bool generate(const ProfileParams& params, bool pathOpen, float detail, int split,
                  int sculptSize = 0)
    {
        detail = std::max(detail, 0.0f);
        points.clear();
        faces.clear();
        const float begin = params.begin;
        const float end = params.end;
        const float hollow = params.hollow;
        if(begin > end - 0.01f)
        {
            return false;
        }

        int faceNum = 0;
        switch(params.curve & ProfileCurve::Mask)
        {
        case ProfileCurve::Square:
        {
            genNGon(params, 4, -0.375f, 1.0f, split);
            if(pathOpen)
            {
                addCap(FaceId::PathBegin);
            }
            for(int i = static_cast<int>(std::floor(begin * 4.0f));
                i < static_cast<int>(std::floor(end * 4.0f + 0.999f)); i++)
            {
                addFace((faceNum++) * (split + 1), split + 2, 1.0f,
                        static_cast<uint16_t>(FaceId::OuterSide0 << i), true);
            }
            for(glm::vec3& p : points)
            {
                p.z *= 4.0f;
            }
            if(hollow > 0.0f)
            {
                switch(params.curve & HoleShape::Mask)
                {
                case HoleShape::Triangle:
                    addHole(params, true, 3, -0.375f, hollow, 1.0f, split);
                    break;
                case HoleShape::Circle:
                    addHole(params, false, kMinDetailFaces * detail, -0.375f, hollow, 1.0f);
                    break;
                default:
                    addHole(params, true, 4, -0.375f, hollow, 1.0f, split);
                    break;
                }
            }
            if(pathOpen)
            {
                faces[0].count = total;
            }
            break;
        }
        case ProfileCurve::IsoTriangle:
        case ProfileCurve::RightTriangle:
        case ProfileCurve::EqualTriangle:
        {
            genNGon(params, 3, 0.0f, 1.0f, split);
            for(glm::vec3& p : points)
            {
                p.z *= 3.0f;
            }
            if(pathOpen)
            {
                addCap(FaceId::PathBegin);
            }
            for(int i = static_cast<int>(std::floor(begin * 3.0f));
                i < static_cast<int>(std::floor(end * 3.0f + 0.999f)); i++)
            {
                addFace((faceNum++) * (split + 1), split + 2, 1.0f,
                        static_cast<uint16_t>(FaceId::OuterSide0 << i), true);
            }
            if(hollow > 0.0f)
            {
                const float triangleHollow = hollow / 2.0f;
                switch(params.curve & HoleShape::Mask)
                {
                case HoleShape::Circle:
                    addHole(params, false, kMinDetailFaces * detail, 0.0f, triangleHollow, 1.0f);
                    break;
                case HoleShape::Square:
                    addHole(params, true, 4, 0.0f, triangleHollow, 1.0f, split);
                    break;
                default:
                    addHole(params, true, 3, 0.0f, triangleHollow, 1.0f, split);
                    break;
                }
            }
            break;
        }
        case ProfileCurve::Circle:
        {
            const uint8_t holeType = params.curve & HoleShape::Mask;
            float circleDetail = kMinDetailFaces * detail;
            if(hollow > 0.0f && holeType == HoleShape::Square)
            {
                circleDetail = std::ceil(circleDetail / 4.0f) * 4.0f;
            }
            genNGon(params, sculptSize > 0 ? sculptSize : static_cast<int>(circleDetail), 0.0f,
                    1.0f, 0);
            if(pathOpen)
            {
                addCap(FaceId::PathBegin);
            }
            addFace(0, open && hollow <= 0.0f ? total - 1 : total, 0.0f, FaceId::OuterSide0, false);
            if(hollow > 0.0f)
            {
                switch(holeType)
                {
                case HoleShape::Square:
                    addHole(params, true, 4, 0.0f, hollow, 1.0f, split);
                    break;
                case HoleShape::Triangle:
                    addHole(params, true, 3, 0.0f, hollow, 1.0f, split);
                    break;
                default:
                    addHole(params, false, circleDetail, 0.0f, hollow, 1.0f);
                    break;
                }
            }
            break;
        }
        case ProfileCurve::HalfCircle:
        {
            const uint8_t holeType = params.curve & HoleShape::Mask;
            float circleDetail = kMinDetailFaces * detail * 0.5f;
            if(hollow > 0.0f && holeType == HoleShape::Square)
            {
                circleDetail = std::ceil(circleDetail / 2.0f) * 2.0f;
            }
            genNGon(params, static_cast<int>(std::floor(circleDetail)), 0.5f, 0.5f, 0);
            if(pathOpen)
            {
                addCap(FaceId::PathBegin);
            }
            addFace(0, open && params.hollow <= 0.0f ? total - 1 : total, 0.0f, FaceId::OuterSide0,
                    false);
            if(hollow > 0.0f)
            {
                switch(holeType)
                {
                case HoleShape::Square:
                    addHole(params, true, 2, 0.5f, hollow, 0.5f, split);
                    break;
                case HoleShape::Triangle:
                    addHole(params, true, 3, 0.5f, hollow, 0.5f, split);
                    break;
                default:
                    addHole(params, false, circleDetail, 0.5f, hollow, 0.5f);
                    break;
                }
            }
            if(params.end - params.begin < 1.0f)
            {
                open = true;
            }
            else if(hollow <= 0.0f)
            {
                open = false;
                points.push_back(points[0]);
                total++;
            }
            break;
        }
        default:
            break;
        }

        if(pathOpen)
        {
            addCap(FaceId::PathEnd);
        }
        if(open)
        {
            // The faces left by a profile cut.
            addFace(total - 1, 2, 0.5f, FaceId::ProfileBegin, true);
            addFace(hollow > 0.0f ? totalOut - 1 : total - 2, 2, 0.5f, FaceId::ProfileEnd, true);
        }
        return true;
    }
};

// --- Path (what the profile is swept along) -------------------------------------

// The official viewer composes "a then b" for a * b; glm's b * a does that.
glm::quat angleAxis(float angle, const glm::vec3& axis)
{
    return glm::angleAxis(angle, axis);
}

struct Path
{
    std::vector<PathPoint> points;
    bool open = true;

    // A circular path from (1, 0, 0), counter-clockwise in the x/z plane.
    void genNGon(const PathParams& params, int sides, float startOff = 0.0f, float endScale = 1.0f,
                 float twistScale = 1.0f)
    {
        (void)startOff;
        constexpr float kTableScale[] = {1, 1, 1, 0.5f, 0.707107f, 0.53f, 0.525f, 0.5f};
        const float revolutions = params.revolutions;
        const float skew = params.skew;
        const float skewMag = std::abs(skew);
        const float holeX = params.scale.x * (1.0f - skewMag);
        const float holeY = params.scale.y;

        // Negative taper tapers the beginning.
        float taperXBegin = 1.0f;
        float taperXEnd = 1.0f - params.taper.x;
        float taperYBegin = 1.0f;
        float taperYEnd = 1.0f - params.taper.y;
        if(taperXEnd > 1.0f)
        {
            taperXBegin = 2.0f - taperXEnd;
            taperXEnd = 1.0f;
        }
        if(taperYEnd > 1.0f)
        {
            taperYBegin = 2.0f - taperYEnd;
            taperYEnd = 1.0f;
        }

        float radiusStart = 0.5f;
        if(sides < 8)
        {
            radiusStart = kTableScale[sides];
        }
        radiusStart *= 1.0f - holeY;
        float radiusEnd = radiusStart;
        const float radiusOffset = params.radiusOffset;
        if(radiusOffset < 0.0f)
        {
            radiusStart *= 1.0f + radiusOffset;
        }
        else
        {
            radiusEnd *= 1.0f - radiusOffset;
        }

        open = (params.end * endScale - params.begin < 1.0f) || skewMag > 0.001f ||
               std::abs(taperXEnd - taperXBegin) > 0.001f ||
               std::abs(taperYEnd - taperYBegin) > 0.001f ||
               std::abs(radiusEnd - radiusStart) > 0.001f;

        const glm::vec3 pathAxis(1.0f, 0.0f, 0.0f);
        const float twistBegin = params.twistBegin * twistScale;
        const float twistEnd = params.twist * twistScale;
        const float step = 1.0f / static_cast<float>(sides);

        auto pointAt = [&](float t)
        {
            const float ang = 2.0f * kPi * revolutions * t;
            const float radius = lerp(radiusStart, radiusEnd, t);
            const float s = std::sin(ang) * radius;
            const float c = std::cos(ang) * radius;
            PathPoint pt;
            pt.position = glm::vec3(lerp(0.0f, params.shear.x, s) + lerp(-skew, skew, t) * 0.5f,
                                    c + lerp(0.0f, params.shear.y, s), s);
            pt.scale = glm::vec2(holeX * lerp(taperXBegin, taperXEnd, t),
                                 holeY * lerp(taperYBegin, taperYEnd, t));
            pt.texT = t;
            const glm::quat twist = angleAxis(lerp(twistBegin, twistEnd, t) * 2.0f * kPi - kPi,
                                              glm::vec3(0.0f, 0.0f, 1.0f));
            const glm::quat around = angleAxis(ang, pathAxis);
            pt.rotation = around * twist; // twist, then around the path
            points.push_back(pt);
        };

        float t = params.begin;
        pointAt(t);
        t += step;
        t = static_cast<float>(static_cast<int>(t * static_cast<float>(sides))) /
            static_cast<float>(sides);
        while(t < params.end)
        {
            pointAt(t);
            t += step;
        }
        pointAt(params.end);
    }

    // sculptSize > 0: a sculpted prim's path, a circle of that many sides.
    bool generate(const PathParams& params, float detail, int split, int sculptSize = 0)
    {
        detail = std::max(detail, 0.0f);
        points.clear();
        open = true;

        switch(params.curve & 0xF0)
        {
        default:
        case PathCurve::Line:
        {
            int np = static_cast<int>(std::floor(std::abs(params.twistBegin - params.twist) * 3.5f *
                                                 (detail - 0.5f))) +
                     2;
            np = std::max(np, split + 2);
            const float step = 1.0f / static_cast<float>(np - 1);
            const glm::vec2 startScale = params.beginScale();
            const glm::vec2 endScale = params.endScale();
            for(int i = 0; i < np; i++)
            {
                const float t = lerp(params.begin, params.end, static_cast<float>(i) * step);
                PathPoint pt;
                pt.position = glm::vec3(lerp(0.0f, params.shear.x, t),
                                        lerp(0.0f, params.shear.y, t), t - 0.5f);
                pt.rotation = angleAxis(lerp(kPi * params.twistBegin, kPi * params.twist, t),
                                        glm::vec3(0, 0, 1));
                pt.scale =
                    glm::vec2(lerp(startScale.x, endScale.x, t), lerp(startScale.y, endScale.y, t));
                pt.texT = t;
                points.push_back(pt);
            }
            break;
        }
        case PathCurve::Circle:
        {
            const float twistMag = std::abs(params.twistBegin - params.twist);
            const int sides =
                sculptSize > 0
                    ? sculptSize
                    : static_cast<int>(std::floor(
                          std::floor(kMinDetailFaces * detail + twistMag * 3.5f * (detail - 0.5f)) *
                          params.revolutions));
            if(sides > 0)
            {
                genNGon(params, sides);
            }
            break;
        }
        case PathCurve::Circle2:
        {
            if(params.end - params.begin >= 0.99f && params.scale.x >= 0.99f)
            {
                open = false;
            }
            genNGon(params, static_cast<int>(std::floor(kMinDetailFaces * detail)));
            float toggle = 0.5f;
            for(PathPoint& pt : points)
            {
                pt.position.x = toggle;
                toggle = toggle == 0.5f ? -0.5f : 0.5f;
            }
            break;
        }
        case PathCurve::Test:
        {
            constexpr int np = 5;
            const float step = 1.0f / static_cast<float>(np - 1);
            for(int i = 0; i < np; i++)
            {
                const float t = static_cast<float>(i) * step;
                PathPoint pt;
                pt.position =
                    glm::vec3(0.0f, lerp(0.0f, -std::sin(kPi * params.twist * t) * 0.5f, t),
                              lerp(-0.5f, std::cos(kPi * params.twist * t) * 0.5f, t));
                pt.scale = glm::vec2(lerp(1.0f, params.scale.x, t), lerp(1.0f, params.scale.y, t));
                pt.texT = t;
                pt.rotation = angleAxis(kPi * params.twist * t, glm::vec3(1, 0, 0));
                points.push_back(pt);
            }
            break;
        }
        }
        if(params.twist != params.twistBegin)
        {
            open = true;
        }
        return true;
    }
};

// --- Faces ----------------------------------------------------------------------------

struct FaceLayout
{
    int beginS = 0;
    int numS = 0;
    int numT = 0;
    uint16_t type = 0;
};

struct Builder
{
    const VolumeParams& params;
    Profile profile;
    Path path;
    std::vector<glm::vec3> mesh; // path.points.size() rows of profile.total points
    // Sculpted prims: how the map's edges join (SculptType; 0 for other
    // prims) and whether its columns run backwards (invert XOR mirror).
    uint8_t sculptStitching = 0;
    bool sculptReversed = false;

    void side(const FaceLayout& layout, VolumeFace& face) const
    {
        const bool flat = layout.type & FaceType::Flat;
        const int maxS = profile.total;
        const auto& pts = profile.points;
        const float beginU = std::floor(pts[static_cast<size_t>(layout.beginS)].z);
        const bool duplicate =
            (layout.type & FaceType::Inner) && (layout.type & FaceType::Flat) && layout.numS > 2;
        const int numS = duplicate ? layout.numS / 2 : layout.numS;

        auto meshIndex = [&](int s, int t)
        {
            return layout.beginS + s >= maxS ? layout.beginS + s + maxS * (t - 1)
                                             : layout.beginS + s + maxS * t;
        };

        for(int t = 0; t < layout.numT; t++)
        {
            const float tt = path.points[static_cast<size_t>(t)].texT;
            for(int s = 0; s < numS; s++)
            {
                float ss = 0.0f;
                if(layout.type & FaceType::End)
                {
                    ss = s ? 1.0f : 0.0f;
                }
                else
                {
                    const int index = layout.beginS + s;
                    if(index >= static_cast<int>(pts.size()))
                    {
                        ss = flat ? 1.0f - beginU : 1.0f;
                    }
                    else
                    {
                        ss = flat ? pts[static_cast<size_t>(index)].z - beginU
                                  : pts[static_cast<size_t>(index)].z;
                    }
                }
                if(sculptReversed)
                {
                    ss = 1.0f - ss;
                }
                const glm::vec3& p = mesh[static_cast<size_t>(meshIndex(s, t))];
                face.positions.push_back(p);
                face.texCoords.push_back({ss, tt});
                if(duplicate && s > 0)
                {
                    face.positions.push_back(p);
                    face.texCoords.push_back({ss, tt});
                }
            }
            if(duplicate)
            {
                const int s = (layout.type & FaceType::Open) ? numS - 1 : 0;
                face.positions.push_back(mesh[static_cast<size_t>(layout.beginS + s + maxS * t)]);
                face.texCoords.push_back(
                    {pts[static_cast<size_t>(layout.beginS + s)].z - beginU, tt});
            }
        }

        const auto stride = static_cast<uint32_t>(layout.numS);
        for(uint32_t t = 0; t + 1 < static_cast<uint32_t>(layout.numT); t++)
        {
            for(uint32_t s = 0; s + 1 < stride; s++)
            {
                face.indices.insert(face.indices.end(),
                                    {s + stride * t, s + 1 + stride * (t + 1), s + stride * (t + 1),
                                     s + stride * t, s + 1 + stride * t, s + 1 + stride * (t + 1)});
            }
        }

        // Area-weighted normals (with the official viewer's small alternating
        // bias), then wrapped across closed seams.
        std::vector<glm::vec3>& n = face.normals;
        n.assign(face.positions.size(), glm::vec3(0.0f));
        for(size_t i = 0; i + 2 < face.indices.size(); i += 3)
        {
            const uint32_t i0 = face.indices[i];
            const uint32_t i1 = face.indices[i + 1];
            const uint32_t i2 = face.indices[i + 2];
            const glm::vec3& p0 = face.positions[i0];
            const glm::vec3 c = glm::cross(p0 - face.positions[i1], p0 - face.positions[i2]);
            n[i0] += c;
            n[i1] += c;
            n[i2] += c;
            ((i / 3) % 2 == 0 ? n[i1] : n[i2]) += c;
        }
        const int rows = layout.numT;
        const int cols = layout.numS;
        auto at = [&](int s, int t) -> glm::vec3& { return n[static_cast<size_t>(s + cols * t)]; };
        const glm::vec3 bottom =
            face.positions[0] - face.positions[static_cast<size_t>(cols * (rows - 2))];
        const bool bottomConverges = glm::dot(bottom, bottom) < 0.000001f;
        const glm::vec3 top = face.positions[static_cast<size_t>(cols - 1)] -
                              face.positions[static_cast<size_t>(cols * (rows - 2) + cols - 1)];
        const bool topConverges = glm::dot(top, top) < 0.000001f;
        if(sculptStitching != 0)
        {
            sculptSeams(cols, rows, n);
            return;
        }
        if(!path.open)
        {
            for(int s = 0; s < cols; s++)
            {
                const glm::vec3 sum = at(s, 0) + at(s, rows - 1);
                at(s, 0) = sum;
                at(s, rows - 1) = sum;
            }
        }
        if(!profile.open && !bottomConverges)
        {
            for(int t = 0; t < rows; t++)
            {
                const glm::vec3 sum = at(0, t) + at(cols - 1, t);
                at(0, t) = sum;
                at(cols - 1, t) = sum;
            }
        }
        if((params.path.curve & 0xF0) == PathCurve::Circle &&
           (params.profile.curve & ProfileCurve::Mask) == ProfileCurve::HalfCircle)
        {
            for(int t = 0; t < rows; t++)
            {
                if(bottomConverges)
                {
                    at(0, t) = glm::vec3(1, 0, 0);
                }
                if(topConverges)
                {
                    at(cols - 1, t) = glm::vec3(-1, 0, 0);
                }
            }
        }
    }

    // Sculpted prims' seams (llvolume.cpp createSide): a sphere's poles
    // share one normal; sphere, torus and cylinder maps wrap their sides
    // together, a torus its top and bottom too.
    void sculptSeams(int cols, int rows, std::vector<glm::vec3>& n) const
    {
        auto at = [&](int s, int t) -> glm::vec3& { return n[static_cast<size_t>(s + cols * t)]; };
        if(sculptStitching == SculptStitching::Sphere)
        {
            for(const int t : {0, rows - 1})
            {
                glm::vec3 average(0.0f);
                for(int s = 0; s < cols; s++)
                {
                    average += at(s, t);
                }
                for(int s = 0; s < cols; s++)
                {
                    at(s, t) = average;
                }
            }
        }
        if(sculptStitching == SculptStitching::Sphere ||
           sculptStitching == SculptStitching::Torus ||
           sculptStitching == SculptStitching::Cylinder)
        {
            for(int t = 0; t < rows; t++)
            {
                const glm::vec3 sum = at(0, t) + at(cols - 1, t);
                at(0, t) = sum;
                at(cols - 1, t) = sum;
            }
        }
        if(sculptStitching == SculptStitching::Torus)
        {
            for(int s = 0; s < cols; s++)
            {
                const glm::vec3 sum = at(s, 0) + at(s, rows - 1);
                at(s, 0) = sum;
                at(s, rows - 1) = sum;
            }
        }
    }

    void cubeCap(const FaceLayout& layout, VolumeFace& face) const
    {
        const auto& pts = profile.points;
        const int maxS = profile.total;
        const int maxT = static_cast<int>(path.points.size());
        const int grid = (static_cast<int>(pts.size()) - 1) / 4;
        const bool top = layout.type & FaceType::Top;
        const int offset = top ? (maxT - 1) * maxS : layout.beginS;

        glm::vec3 corner[4];
        glm::vec2 cornerUv[4];
        for(int t = 0; t < 4; t++)
        {
            corner[t] = mesh[static_cast<size_t>(offset + grid * t)];
            const glm::vec3& p = pts[static_cast<size_t>(grid * t)];
            cornerUv[t] = glm::vec2(p.x + 0.5f, 0.5f - p.y);
        }
        glm::vec3 normal = glm::normalize(glm::cross(corner[1] - corner[0], corner[2] - corner[1]));
        if(!top)
        {
            normal = -normal;
        }
        else
        {
            std::swap(cornerUv[0], cornerUv[3]);
            std::swap(cornerUv[1], cornerUv[2]);
        }

        for(int gx = 0; gx <= grid; gx++)
        {
            for(int gy = 0; gy <= grid; gy++)
            {
                const float a = static_cast<float>(gx) / static_cast<float>(grid);
                const float b = static_cast<float>(gy) / static_cast<float>(grid);
                face.positions.push_back(corner[0] + (corner[1] - corner[0]) * a +
                                         (corner[3] - corner[0]) * b);
                face.texCoords.push_back(cornerUv[0] + (cornerUv[1] - cornerUv[0]) * a +
                                         (cornerUv[3] - cornerUv[0]) * b);
                face.normals.push_back(normal);
            }
        }
        const auto g = static_cast<uint32_t>(grid);
        const uint32_t quad[] = {0, 1, (g + 1) + 1, (g + 1) + 1, g + 1, 0};
        for(uint32_t gx = 0; gx < g; gx++)
        {
            for(uint32_t gy = 0; gy < g; gy++)
            {
                const uint32_t base = gy * (g + 1) + gx;
                for(int i = 0; i < 6; i++)
                {
                    face.indices.push_back(base + quad[top ? 5 - i : i]);
                }
            }
        }
    }

    void cap(const FaceLayout& layout, VolumeFace& face) const
    {
        const bool hollow = layout.type & FaceType::Hollow;
        const bool open = layout.type & FaceType::Open;
        if(!hollow && !open && params.path.begin == 0.0f && params.path.end == 1.0f &&
           params.profile.curve == ProfileCurve::Square && params.path.curve == PathCurve::Line)
        {
            cubeCap(layout, face);
            return;
        }

        const auto& pts = profile.points;
        const int maxS = profile.total;
        const int maxT = static_cast<int>(path.points.size());
        const bool top = layout.type & FaceType::Top;
        const int offset = top ? (maxT - 1) * maxS : layout.beginS;
        int count = static_cast<int>(pts.size());

        glm::vec3 low = mesh[static_cast<size_t>(offset)];
        glm::vec3 high = low;
        glm::vec2 lowUv(0.0f);
        glm::vec2 highUv(0.0f);
        for(int i = 0; i < count; i++)
        {
            const glm::vec3& p = mesh[static_cast<size_t>(offset + i)];
            const glm::vec3& q = pts[static_cast<size_t>(i)];
            const glm::vec2 uv =
                top ? glm::vec2(q.x + 0.5f, q.y + 0.5f) : glm::vec2(q.x + 0.5f, 0.5f - q.y);
            if(i == 0)
            {
                lowUv = highUv = uv;
            }
            low = glm::min(low, p);
            high = glm::max(high, p);
            lowUv = glm::min(lowUv, uv);
            highUv = glm::max(highUv, uv);
            face.positions.push_back(p);
            face.texCoords.push_back(uv);
        }
        if(!hollow && !open)
        {
            face.positions.push_back((low + high) * 0.5f);
            face.texCoords.push_back((lowUv + highUv) * 0.5f);
            count++;
        }

        auto& idx = face.indices;
        if(hollow)
        {
            // Zip the outer and inner outlines together from both ends.
            int pt1 = 0;
            int pt2 = count - 1;
            auto area = [](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
            { return (a.x * b.y - b.x * a.y) + (b.x * c.y - c.x * b.y) + (c.x * a.y - a.x * c.y); };
            while(pt2 - pt1 > 1)
            {
                const glm::vec3& p1 = pts[static_cast<size_t>(pt1)];
                const glm::vec3& p2 = pts[static_cast<size_t>(pt2)];
                const glm::vec3& pa = pts[static_cast<size_t>(pt1 + 1)];
                const glm::vec3& pb = pts[static_cast<size_t>(pt2 - 1)];
                const bool tri1a2 = area(p1, pa, p2) >= 0 && area(p2, pa, pb) >= 0;
                const bool tri21b = area(p2, p1, pb) >= 0 && area(p1, pb, pa) >= 0;
                bool use1a2 = true;
                if(!tri1a2)
                {
                    use1a2 = false;
                }
                else if(tri21b)
                {
                    const glm::vec3 d1 = p1 - pa;
                    const glm::vec3 d2 = p2 - pb;
                    use1a2 = glm::dot(d1, d1) < glm::dot(d2, d2);
                }
                const auto u1 = static_cast<uint32_t>(pt1);
                const auto u2 = static_cast<uint32_t>(pt2);
                if(use1a2)
                {
                    if(top)
                    {
                        idx.insert(idx.end(), {u1, u1 + 1, u2});
                    }
                    else
                    {
                        idx.insert(idx.end(), {u1, u2, u1 + 1});
                    }
                    pt1++;
                }
                else
                {
                    if(top)
                    {
                        idx.insert(idx.end(), {u1, u2 - 1, u2});
                    }
                    else
                    {
                        idx.insert(idx.end(), {u1, u2, u2 - 1});
                    }
                    pt2--;
                }
            }
        }
        else
        {
            // A fan around the center (the last point).
            const auto center = static_cast<uint32_t>(count - 1);
            for(uint32_t i = 0; i + 2 < static_cast<uint32_t>(count); i++)
            {
                if(top)
                {
                    idx.insert(idx.end(), {center, i, i + 1});
                }
                else
                {
                    idx.insert(idx.end(), {center, i + 1, i});
                }
            }
        }

        glm::vec3 normal(0.0f, 0.0f, 1.0f);
        if(idx.size() >= 3)
        {
            const glm::vec3 n = glm::cross(face.positions[idx[1]] - face.positions[idx[0]],
                                           face.positions[idx[2]] - face.positions[idx[0]]);
            if(glm::dot(n, n) > 1e-12f)
            {
                normal = glm::normalize(n);
            }
            else
            {
                normal = glm::vec3(0.0f, 0.0f, n.z >= 0.0f ? 1.0f : -1.0f);
            }
        }
        face.normals.assign(face.positions.size(), normal);
    }
};

} // namespace

glm::vec2 PathParams::beginScale() const
{
    glm::vec2 s(1.0f);
    if(scale.x > 1.0f)
    {
        s.x = 2.0f - scale.x;
    }
    if(scale.y > 1.0f)
    {
        s.y = 2.0f - scale.y;
    }
    return s;
}

glm::vec2 PathParams::endScale() const
{
    glm::vec2 s(1.0f);
    if(scale.x < 1.0f)
    {
        s.x = scale.x;
    }
    if(scale.y < 1.0f)
    {
        s.y = scale.y;
    }
    return s;
}

VolumeParams unpackVolumeParams(const PackedVolumeParams& p)
{
    // Quantization as llvolume.h / llvolumemessage.cpp.
    constexpr float kCut = 0.00002f;
    constexpr float kScale = 0.01f;
    constexpr float kShear = 0.01f;
    constexpr float kTaper = 0.01f;
    constexpr float kRevolutions = 0.015f;
    constexpr float kHollow = 0.00002f;

    VolumeParams v;
    v.path.curve = p.pathCurve;
    v.path.begin = static_cast<float>(p.pathBegin) * kCut;
    v.path.end = static_cast<float>(50000 - p.pathEnd) * kCut;
    v.path.scale = glm::vec2(static_cast<float>(200 - p.pathScaleX) * kScale,
                             static_cast<float>(200 - p.pathScaleY) * kScale);
    v.path.shear = glm::vec2(static_cast<float>(p.pathShearX) * kShear,
                             static_cast<float>(p.pathShearY) * kShear);
    v.path.twist = static_cast<float>(p.pathTwist) * kScale;
    v.path.twistBegin = static_cast<float>(p.pathTwistBegin) * kScale;
    v.path.radiusOffset = static_cast<float>(p.pathRadiusOffset) * kScale;
    v.path.taper = glm::vec2(static_cast<float>(p.pathTaperX) * kTaper,
                             static_cast<float>(p.pathTaperY) * kTaper);
    v.path.revolutions = static_cast<float>(p.pathRevolutions) * kRevolutions + 1.0f;
    v.path.skew = static_cast<float>(p.pathSkew) * kScale;

    v.profile.curve = p.profileCurve;
    v.profile.begin = static_cast<float>(p.profileBegin) * kCut;
    if(v.profile.begin > 1.0f)
    {
        v.profile.begin = 0.0f;
    }
    const float endCut = static_cast<float>(p.profileEnd) * kCut;
    v.profile.end = endCut > 1.0f ? 1.0f : 1.0f - endCut;
    v.profile.hollow = static_cast<float>(p.profileHollow) * kHollow;
    if(v.profile.hollow > 1.0f)
    {
        v.profile.hollow = 0.0f;
    }
    return v;
}

// Each profile face's triangles over the mesh.
static std::vector<VolumeFace> buildFaces(const Builder& b, const VolumeParams& effective)
{
    std::vector<VolumeFace> faces(b.profile.faces.size());
    for(size_t i = 0; i < b.profile.faces.size(); i++)
    {
        const ProfileFace& pf = b.profile.faces[i];
        FaceLayout layout;
        layout.beginS = pf.index;
        layout.numS = pf.count;
        layout.numT = static_cast<int>(b.path.points.size());
        if(effective.profile.hollow > 0.0f)
        {
            layout.type |= FaceType::Hollow;
        }
        if(b.profile.open)
        {
            layout.type |= FaceType::Open;
        }
        if(pf.cap)
        {
            layout.type |=
                FaceType::Cap | (pf.id == FaceId::PathBegin ? FaceType::Top : FaceType::Bottom);
        }
        else if(pf.id & (FaceId::ProfileBegin | FaceId::ProfileEnd))
        {
            layout.type |= FaceType::Flat | FaceType::End;
        }
        else
        {
            layout.type |= FaceType::Side;
            if(pf.flat)
            {
                layout.type |= FaceType::Flat;
            }
            if(pf.id & FaceId::InnerSide)
            {
                layout.type |= FaceType::Inner;
                if(pf.flat && layout.numS > 2)
                {
                    layout.numS *= 2; // flat inner faces duplicate vertices for hard edges
                }
            }
            else
            {
                layout.type |= FaceType::Outer;
            }
        }

        VolumeFace& face = faces[i];
        face.type = layout.type;
        if(layout.numS < 1)
        {
            continue;
        }
        if(layout.type & FaceType::Cap)
        {
            b.cap(layout, face);
        }
        else
        {
            b.side(layout, face);
        }
        for(glm::vec3& n : face.normals)
        {
            const float length = glm::length(n);
            n = length > 1e-12f ? n / length : glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }
    return faces;
}

// Sweep: each profile point scaled by the path's taper, rotated and moved
// to each path point; then the faces.
static std::vector<VolumeFace> sweep(Builder& b, const VolumeParams& params)
{
    b.mesh.reserve(b.path.points.size() * static_cast<size_t>(b.profile.total));
    for(const PathPoint& pp : b.path.points)
    {
        for(const glm::vec3& p : b.profile.points)
        {
            b.mesh.push_back(pp.rotation * glm::vec3(p.x * pp.scale.x, p.y * pp.scale.y, 0.0f) +
                             pp.position);
        }
    }
    return buildFaces(b, params);
}

std::vector<VolumeFace> generateVolume(const VolumeParams& params, float detail)
{
    Builder b{params, {}, {}, {}};

    int split = static_cast<int>(detail * 0.66f);
    const uint8_t profileCurve = params.profile.curve;
    if(params.path.curve == PathCurve::Line &&
       (params.path.scale.x != 1.0f || params.path.scale.y != 1.0f) &&
       (profileCurve == ProfileCurve::Square || profileCurve == ProfileCurve::IsoTriangle ||
        profileCurve == ProfileCurve::EqualTriangle || profileCurve == ProfileCurve::RightTriangle))
    {
        split = 0;
    }

    // Flexible prims bend at runtime (generateVolumeAlong); straight here.
    VolumeParams effective = params;
    if((effective.path.curve & PathCurve::Flexible) != 0)
    {
        effective.path.curve = PathCurve::Line;
    }

    b.path.generate(effective.path, detail, split);
    if(b.path.points.size() < 2 ||
       !b.profile.generate(effective.profile, b.path.open, detail, split))
    {
        return {};
    }
    if(b.profile.total < 2)
    {
        return {};
    }
    return sweep(b, effective);
}

std::vector<VolumeFace> generateVolumeAlong(const VolumeParams& params, float detail,
                                            std::span<const PathPoint> path)
{
    Builder b{params, {}, {}, {}};
    VolumeParams effective = params;
    effective.path.curve = PathCurve::Line; // faces laid out as a line's
    b.path.points.assign(path.begin(), path.end());
    b.path.open = true;
    const int split = static_cast<int>(detail * 0.66f);
    if(b.path.points.size() < 2 ||
       !b.profile.generate(effective.profile, b.path.open, detail, split) || b.profile.total < 2)
    {
        return {};
    }
    return sweep(b, effective);
}

std::vector<VolumeFace> generateSculpt(const VolumeParams& params, float detail, uint8_t sculptType,
                                       const SculptMap& map)
{
    // Mesh resolution (sculpt_calc_mesh_resolution): as close to the map's
    // aspect as possible, at most a side of 6, 8, 16 or 32 points squared by
    // level of detail, and at most a vertex per four pixels.
    const int lodSides = detail <= 1.0f ? 6 : detail <= 2.0f ? 8 : detail <= 3.0f ? 16 : 32;
    const int mapVertices = static_cast<int>(map.width * map.height / 4);
    const int vertices =
        mapVertices > 0 ? std::min(lodSides * lodSides, mapVertices) : lodSides * lodSides;
    const float ratio = map.width == 0 || map.height == 0
                            ? 1.0f
                            : static_cast<float>(map.width) / static_cast<float>(map.height);
    int sizeS = std::max(static_cast<int>(std::sqrt(static_cast<float>(vertices) / ratio)), 4);
    const int sizeT = std::max(vertices / sizeS, 4);
    sizeS = vertices / sizeT;

    Builder b{params, {}, {}, {}};
    b.sculptStitching = sculptType & SculptStitching::Mask;
    const bool invert = sculptType & SculptStitching::Invert;
    const bool mirror = sculptType & SculptStitching::Mirror;
    b.sculptReversed = invert != mirror;
    b.path.generate(params.path, detail, 0, sizeS);
    if(b.path.points.size() < 2 ||
       !b.profile.generate(params.profile, b.path.open, detail, 0, sizeT) || b.profile.total < 2)
    {
        return {};
    }
    const int rows = static_cast<int>(b.path.points.size());
    const int cols = static_cast<int>(b.profile.points.size());
    b.mesh.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols));

    const bool usable = map.width > 0 && map.height > 0 &&
                        map.rgba.size() >= static_cast<size_t>(map.width) * map.height * 4;
    // A map's pixel to a point: 0..255 to -0.5..0.5 (sculpt_rgb_to_vector).
    auto pixel = [&](uint32_t x, uint32_t y)
    {
        const size_t i = (static_cast<size_t>(x) + static_cast<size_t>(y) * map.width) * 4;
        return glm::vec3(map.rgba[i], map.rgba[i + 1], map.rgba[i + 2]) / 255.0f - glm::vec3(0.5f);
    };
    // Sample along the profile (x) and path (y), joining edges by type
    // (sculptGenerateMapVertices).
    for(int s = 0; s < rows && usable; s++)
    {
        for(int t = 0; t < cols; t++)
        {
            const int column = b.sculptReversed ? cols - t - 1 : t;
            auto x =
                static_cast<uint32_t>(static_cast<float>(column) / static_cast<float>(cols - 1) *
                                      static_cast<float>(map.width));
            auto y = static_cast<uint32_t>(static_cast<float>(s) / static_cast<float>(rows - 1) *
                                           static_cast<float>(map.height));
            if(y == 0 && b.sculptStitching == SculptStitching::Sphere)
            {
                x = map.width / 2; // pinched
            }
            if(y == map.height)
            {
                y = b.sculptStitching == SculptStitching::Torus ? 0 : map.height - 1;
                if(b.sculptStitching == SculptStitching::Sphere)
                {
                    x = map.width / 2;
                }
            }
            if(x == map.width)
            {
                const bool wraps = b.sculptStitching == SculptStitching::Sphere ||
                                   b.sculptStitching == SculptStitching::Torus ||
                                   b.sculptStitching == SculptStitching::Cylinder;
                x = wraps ? 0 : map.width - 1;
            }
            glm::vec3 p = pixel(x, y);
            if(mirror)
            {
                p.x = -p.x;
            }
            b.mesh[static_cast<size_t>(s * cols + t)] = p;
        }
    }

    // A map without enough shape (or too much: a broken one) makes a sphere
    // instead (sculptGetSurfaceArea; not checked at the lowest detail, for
    // old content).
    bool placeholder = !usable;
    if(usable && detail > 1.0f)
    {
        float area = 0.0f;
        for(int s = 0; s + 1 < rows; s++)
        {
            for(int t = 0; t + 1 < cols; t++)
            {
                const glm::vec3& p1 = b.mesh[static_cast<size_t>(s * cols + t)];
                const glm::vec3& p2 = b.mesh[static_cast<size_t>((s + 1) * cols + t)];
                const glm::vec3& p3 = b.mesh[static_cast<size_t>(s * cols + t + 1)];
                const glm::vec3& p4 = b.mesh[static_cast<size_t>((s + 1) * cols + t + 1)];
                area += (glm::length(glm::cross(p1 - p2, p1 - p3)) +
                         glm::length(glm::cross(p4 - p2, p4 - p3))) /
                        2.0f;
            }
        }
        placeholder = area < 0.002f || area > 384.0f;
    }
    if(placeholder)
    {
        for(int s = 0; s < rows; s++)
        {
            for(int t = 0; t < cols; t++)
            {
                const float u = static_cast<float>(s) / static_cast<float>(rows - 1);
                const float v = static_cast<float>(t) / static_cast<float>(cols - 1);
                constexpr float kRadius = 0.3f;
                const float pi = glm::pi<float>();
                b.mesh[static_cast<size_t>(s * cols + t)] =
                    glm::vec3(std::sin(pi * v) * std::cos(2.0f * pi * u),
                              std::sin(pi * v) * std::sin(2.0f * pi * u), std::cos(pi * v)) *
                    kRadius;
            }
        }
    }
    return buildFaces(b, params);
}

} // namespace viewer::volume
