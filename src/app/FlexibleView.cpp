#include "FlexibleView.hpp"

#include "FaceUpload.hpp"

#include "viewer/volume/Volume.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxTension = 0.99f; // FLEXIBLE_OBJECT_MAX_INTERNAL_TENSION_FORCE
constexpr float kMaxStep = 0.2f;     // seconds
// Forgotten once not drawn for this many frames.
constexpr uint64_t kForgetAfter = 600;
// The prim jumped this far (a teleport, the scene's origin moving): start
// over rather than swing.
constexpr float kJump = 32.0f;

// The shortest rotation from one direction to another (unit vectors).
glm::quat shortestArc(const glm::vec3& from, const glm::vec3& to)
{
    const float c = glm::dot(from, to);
    if(c < -0.99999f)
    {
        // Opposite: half a turn about any perpendicular.
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        if(glm::dot(axis, axis) < 1e-6f)
        {
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), from);
        }
        return glm::angleAxis(kPi, glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + c, axis.x, axis.y, axis.z));
}

template <typename T>
T lerp(const T& a, const T& b, float t)
{
    return a + (b - a) * t;
}

} // namespace

FlexibleView::FlexibleView(render::Renderer& renderer) : renderer_(renderer) {}

void FlexibleView::remap(const Sections& source, int sourceResolution, Sections& out,
                         int resolution, float length)
{
    const int count = 1 << resolution;
    const float sectionLength = length / static_cast<float>(count);
    if(sourceResolution < 0)
    {
        // All from section 0, straight up its direction.
        out[0] = source[0];
        for(int i = 0; i < count; i++)
        {
            out[i + 1] = out[i];
            out[i + 1].position += out[i].direction * sectionLength;
            out[i + 1].velocity = glm::vec3(0.0f);
        }
    }
    else if(sourceResolution > resolution)
    {
        // Every so many.
        const int stride = 1 << (sourceResolution - resolution);
        for(int i = 0; i < count; i++)
        {
            out[i + 1] = source[(i + 1) * stride];
        }
        out[0] = source[0];
    }
    else if(sourceResolution < resolution)
    {
        // Cubic in position between source sections (from their positions
        // and derivatives), the rest blended; right to left so it can work
        // in place.
        const int shift = resolution - sourceResolution;
        const int steps = 1 << shift;
        const float sourceLength = length / static_cast<float>(1 << sourceResolution);
        for(int i = count - steps; i >= 0; i -= steps)
        {
            const Section last = source[i >> shift];
            const Section next = source[(i >> shift) + 1];
            const glm::vec3 d = last.position;
            const glm::vec3 c = last.dPosition * sourceLength;
            const glm::vec3 y = next.dPosition * sourceLength - c;
            const glm::vec3 x = next.position - d - c;
            const glm::vec3 a = y - 2.0f * x;
            const glm::vec3 b = x - a;
            const float step = 1.0f / static_cast<float>(steps);
            float t = step;
            for(int k = 1; k < steps; k++)
            {
                Section& s = out[i + k];
                s.scale = lerp(last.scale, next.scale, t);
                s.axisRotation = glm::slerp(last.axisRotation, next.axisRotation, t);
                s.position = t * t * (t * a + b) + t * c + d;
                s.rotation = glm::slerp(last.rotation, next.rotation, t);
                s.velocity = lerp(last.velocity, next.velocity, t);
                s.direction = lerp(last.direction, next.direction, t);
                s.dPosition = lerp(last.dPosition, next.dPosition, t);
                t += step;
            }
            out[i + steps] = next;
        }
        out[0] = source[0];
    }
    else
    {
        for(int i = 0; i <= count; i++)
        {
            out[i] = source[i];
        }
    }
}

void FlexibleView::setAttributes(Prim& prim, const glm::vec3& position, const glm::quat& rotation)
{
    const volume::PathParams& path = prim.volume.path;
    const glm::vec2 bottom = path.beginScale();
    const glm::vec2 top = path.endScale();
    const float beginTwist = kPi * path.twistBegin;
    const float endTwist = kPi * path.twist;
    const glm::vec2 size(prim.scale.x, prim.scale.y);
    const glm::vec3 up = rotation * glm::vec3(0.0f, 0.0f, 1.0f);

    Section& first = prim.sections[0];
    first.position = position - prim.scale.z * 0.5f * up; // the anchor: its bottom
    first.direction = up;
    first.dPosition = up;
    first.scale = size * bottom;
    first.velocity = glm::vec3(0.0f);
    first.axisRotation = glm::angleAxis(beginTwist, glm::vec3(0.0f, 0.0f, 1.0f));

    remap(prim.sections, prim.initializedResolution, prim.sections, prim.simulateResolution,
          prim.scale.z);
    prim.initializedResolution = prim.simulateResolution;

    const int count = 1 << prim.simulateResolution;
    const float step = 1.0f / static_cast<float>(count);
    float t = step;
    for(int i = 1; i <= count; i++)
    {
        prim.sections[i].axisRotation =
            glm::angleAxis(lerp(beginTwist, endTwist, t), glm::vec3(0.0f, 0.0f, 1.0f));
        prim.sections[i].scale = size * lerp(bottom, top, t);
        t += step;
    }
}

void FlexibleView::simulate(Prim& prim, const glm::vec3& position, const glm::quat& rotation,
                            float seconds)
{
    const world::FlexibleParams& f = prim.params;
    const int count = 1 << prim.simulateResolution;
    Sections& s = prim.sections;
    const float length = prim.scale.z / static_cast<float>(count);
    const float inverseLength = 1.0f / length;

    const glm::vec3 up = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    s[0].position = position - prim.scale.z * 0.5f * up;
    s[0].direction = up;
    s[0].rotation = rotation;
    glm::quat parentRotation = rotation;

    // The same for every section.
    const float tension =
        std::min(f.tension * 0.1f * (1.0f - std::pow(0.85f, seconds * 30.0f)), kMaxTension);
    const float friction = std::max(std::pow(10.0f, (f.drag * 2.0f + 1.0f) * seconds), 1.0f);
    const float momentum = 1.0f / friction;
    const float maxAngle = std::atan(length * 2.0f);
    const float force = length * seconds;

    for(int i = 1; i <= count; i++)
    {
        const glm::vec3 last = s[i].position;
        s[i].position.z -= f.gravity * force;
        s[i].position += f.force * force;

        // Tension: toward where the section below points.
        const glm::vec3 parentPosition = s[i - 1].position;
        const glm::vec3 parentDirection = s[i - 1].direction;
        const glm::vec3 parentVector = i == 1 ? s[0].direction : s[i - 2].direction;
        const glm::vec3 current = s[i].position - parentPosition;
        s[i].position += (parentVector * length - current) * tension;

        // Inertia.
        s[i].position += s[i].velocity * momentum;

        // Length kept, bending limited.
        glm::vec3 direction = s[i].position - parentPosition;
        const float directionLength = glm::length(direction);
        direction = directionLength > 1e-6f ? direction / directionLength : parentDirection;
        glm::quat delta = shortestArc(parentDirection, direction);
        float angle = glm::angle(delta);
        glm::vec3 axis = glm::axis(delta);
        if(angle > kPi)
        {
            angle -= 2.0f * kPi;
        }
        if(angle > maxAngle)
        {
            delta = glm::angleAxis(maxAngle, axis);
        }
        else if(angle < -maxAngle)
        {
            delta = glm::angleAxis(-maxAngle, axis);
        }
        const glm::quat segment = delta * parentRotation;
        parentRotation = segment;
        s[i].direction = delta * parentDirection;
        s[i].position = parentPosition + s[i].direction * length;
        s[i].rotation = segment;
        if(i > 1)
        {
            // Half the bend goes to the section below.
            s[i - 1].rotation = glm::angleAxis(angle * 0.5f, axis) * s[i - 1].rotation;
        }

        s[i].velocity = s[i].position - last;
        if(glm::dot(s[i].velocity, s[i].velocity) > 1.0f)
        {
            s[i].velocity = glm::normalize(s[i].velocity);
        }
    }

    // Derivatives along the length (quadratic through the neighbours).
    s[0].dPosition = (s[1].position - s[0].position) * inverseLength;
    for(int i = 1; i < count; i++)
    {
        const glm::vec3 a =
            (s[i - 1].position - s[i].position + s[i + 1].position - s[i].position) * 0.5f *
            inverseLength * inverseLength;
        s[i].dPosition =
            (s[i + 1].position - s[i].position - a * (length * length)) * inverseLength;
    }
    s[count].dPosition = (s[count].position - s[count - 1].position) * inverseLength;
}

const std::vector<render::Mesh>* FlexibleView::faces(const world::ObjectKey& key,
                                                     const world::Object& object,
                                                     const glm::mat4& frame, float distance,
                                                     int detail, Clock::time_point now)
{
    if(!object.flexible || object.scale.z <= 0.0f)
    {
        return nullptr;
    }
    Prim& prim = prims_[key];
    const glm::vec3 position(frame[3]);
    const glm::quat rotation = glm::quat_cast(glm::mat3(frame));
    if(prim.params != *object.flexible || prim.scale != object.scale ||
       prim.volume != object.volume ||
       (prim.initialized && glm::length(prim.sections[0].position - position) > kJump))
    {
        prim.params = *object.flexible;
        prim.scale = object.scale;
        prim.volume = object.volume;
        prim.initialized = false;
        prim.initializedResolution = -1;
    }
    prim.drawnFrame = frame_;

    // As many sections as it needs on screen (more the bigger it looks),
    // and no more simulated than drawn (updateRenderRes).
    const int softness = std::clamp(prim.params.softness, 0, kMaxResolution);
    prim.renderResolution =
        std::clamp(static_cast<int>(12.0f * object.scale.z / std::max(distance, 0.01f)),
                   std::max(softness - 1, 0), kMaxResolution);
    const int simulated = std::min(softness, prim.renderResolution);
    if(!prim.initialized || prim.simulateResolution != simulated)
    {
        prim.simulateResolution = simulated;
        setAttributes(prim, position, rotation);
        prim.initialized = true;
        prim.lastStep = now;
    }

    const float seconds =
        std::clamp(std::chrono::duration<float>(now - prim.lastStep).count(), 0.0f, kMaxStep);
    prim.lastStep = now;
    simulate(prim, position, rotation, seconds);

    // The drawn sections, as path points in the prim's frame (meters).
    Sections drawn{};
    remap(prim.sections, prim.simulateResolution, drawn, prim.renderResolution, object.scale.z);
    const int count = 1 << prim.renderResolution;
    const glm::quat toPrim = glm::inverse(rotation);
    std::vector<volume::PathPoint> path(static_cast<size_t>(count) + 1);
    for(int i = 0; i <= count; i++)
    {
        volume::PathPoint& p = path[static_cast<size_t>(i)];
        p.position = toPrim * (drawn[i].position - position);
        p.rotation = toPrim * drawn[i].rotation * drawn[i].axisRotation;
        p.scale = drawn[i].scale;
        p.texT = static_cast<float>(i) / static_cast<float>(count);
    }
    const auto faces = volume::generateVolumeAlong(
        object.volume, volume::kDetailScales[static_cast<size_t>(std::clamp(detail, 0, 3))], path);
    if(faces.empty())
    {
        return nullptr;
    }
    prim.meshes = uploadFaces(renderer_, faces);
    return &prim.meshes;
}

void FlexibleView::endFrame()
{
    frame_++;
    std::erase_if(prims_, [&](const auto& entry)
                  { return frame_ - entry.second.drawnFrame > kForgetAfter; });
}

} // namespace viewer
