#include "viewer/avatar/Animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace viewer::avatar
{

namespace
{

constexpr uint16_t kVersion = 1;
constexpr uint16_t kSubVersion = 0;
constexpr int kHighestPriority = 4; // LLJoint::HIGHEST_PRIORITY
constexpr float kMaxPelvisOffset = 5.0f;
constexpr float kMaxDuration = 60.0f;
constexpr uint32_t kMaxJoints = 216;

class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    template <typename T>
    bool read(T& out)
    {
        if(offset_ + sizeof(T) > data_.size())
        {
            return false;
        }
        std::memcpy(&out, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }
    // A zero-terminated string.
    bool readString(std::string& out)
    {
        const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset_);
        const auto end = std::find(begin, data_.end(), uint8_t{0});
        if(end == data_.end())
        {
            return false;
        }
        out.assign(begin, end);
        offset_ += out.size() + 1;
        return true;
    }

private:
    std::span<const uint8_t> data_;
    size_t offset_ = 0;
};

// As the official viewer's U16_to_F32, with values within half a step of
// zero snapped to zero.
float fromU16(uint16_t value, float lower, float upper)
{
    const float delta = upper - lower;
    const float v = static_cast<float>(value) / 65535.0f * delta + lower;
    return std::fabs(v) < delta / 65535.0f ? 0.0f : v;
}

float cubicStep(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// LL's nlerp(t, a, b): a towards b by t, normalized, along the short way.
glm::quat nlerp(float t, const glm::quat& a, glm::quat b)
{
    if(glm::dot(a, b) < 0.0f)
    {
        b = -b;
    }
    return glm::normalize(a * (1.0f - t) + b * t);
}

template <typename Key>
std::pair<const Key*, float> bracket(const std::vector<Key>& keys, float time)
{
    // The last key at or before `time`, and how far towards the next one.
    const auto right = std::ranges::upper_bound(keys, time, {}, &Key::time);
    if(right == keys.begin())
    {
        return {&keys.front(), 0.0f};
    }
    if(right == keys.end())
    {
        return {&keys.back(), 0.0f};
    }
    const Key& left = *(right - 1);
    const float span = right->time - left.time;
    return {&left, span > 0.0f ? (time - left.time) / span : 0.0f};
}

} // namespace

std::expected<Animation, std::string> parseAnimation(std::span<const uint8_t> data)
{
    Reader r(data);
    Animation a;
    uint16_t version = 0;
    uint16_t subVersion = 0;
    if(!r.read(version) || !r.read(subVersion))
    {
        return std::unexpected(std::string("truncated header"));
    }
    if(version != kVersion || subVersion != kSubVersion)
    {
        return std::unexpected(std::format("unsupported version {}.{}", version, subVersion));
    }
    int32_t priority = 0;
    int32_t loop = 0;
    if(!r.read(priority) || !r.read(a.duration) || !r.readString(a.emote) || !r.read(a.loopIn) ||
       !r.read(a.loopOut) || !r.read(loop) || !r.read(a.easeIn) || !r.read(a.easeOut) ||
       !r.read(a.handPose))
    {
        return std::unexpected(std::string("truncated header"));
    }
    if(!std::isfinite(a.duration) || a.duration < 0.0f || a.duration > kMaxDuration ||
       !std::isfinite(a.loopIn) || !std::isfinite(a.loopOut) || !std::isfinite(a.easeIn) ||
       !std::isfinite(a.easeOut))
    {
        return std::unexpected(std::string("bad timing"));
    }
    a.basePriority = std::clamp(static_cast<int>(priority), 0, kHighestPriority);
    a.loop = loop != 0;

    uint32_t jointCount = 0;
    if(!r.read(jointCount) || jointCount == 0 || jointCount > kMaxJoints)
    {
        return std::unexpected(std::format("bad joint count {}", jointCount));
    }
    for(uint32_t j = 0; j < jointCount; j++)
    {
        Animation::JointCurve curve;
        int32_t jointPriority = 0;
        int32_t keys = 0;
        if(!r.readString(curve.name) || !r.read(jointPriority) || !r.read(keys) || keys < 0)
        {
            return std::unexpected(std::string("truncated joint"));
        }
        // -1: the animation's priority.
        curve.priority = jointPriority < 0
                             ? a.basePriority
                             : std::min(static_cast<int>(jointPriority), kHighestPriority);
        for(int32_t k = 0; k < keys; k++)
        {
            uint16_t t = 0;
            uint16_t x = 0;
            uint16_t y = 0;
            uint16_t z = 0;
            if(!r.read(t) || !r.read(x) || !r.read(y) || !r.read(z))
            {
                return std::unexpected(std::string("truncated rotation key"));
            }
            // The quaternion's xyz; w makes it unit length.
            const glm::vec3 v(fromU16(x, -1.0f, 1.0f), fromU16(y, -1.0f, 1.0f),
                              fromU16(z, -1.0f, 1.0f));
            const float w2 = 1.0f - glm::dot(v, v);
            curve.rotations.push_back({fromU16(t, 0.0f, a.duration),
                                       glm::quat(w2 > 0.0f ? std::sqrt(w2) : 0.0f, v.x, v.y, v.z)});
        }
        if(!r.read(keys) || keys < 0)
        {
            return std::unexpected(std::string("truncated joint"));
        }
        for(int32_t k = 0; k < keys; k++)
        {
            uint16_t t = 0;
            uint16_t x = 0;
            uint16_t y = 0;
            uint16_t z = 0;
            if(!r.read(t) || !r.read(x) || !r.read(y) || !r.read(z))
            {
                return std::unexpected(std::string("truncated position key"));
            }
            curve.positions.push_back({fromU16(t, 0.0f, a.duration),
                                       glm::vec3(fromU16(x, -kMaxPelvisOffset, kMaxPelvisOffset),
                                                 fromU16(y, -kMaxPelvisOffset, kMaxPelvisOffset),
                                                 fromU16(z, -kMaxPelvisOffset, kMaxPelvisOffset))});
        }
        // Keys at the same time: the last one wins (as the official viewer's
        // map insert).
        auto dedupe = [](auto& list)
        {
            std::ranges::stable_sort(list, {}, [](const auto& key) { return key.time; });
            for(size_t i = 1; i < list.size();)
            {
                if(list[i].time == list[i - 1].time)
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i - 1));
                }
                else
                {
                    i++;
                }
            }
        };
        dedupe(curve.rotations);
        dedupe(curve.positions);
        // mScreen and mRoot aren't animated.
        if(curve.name != "mScreen" && curve.name != "mRoot")
        {
            a.joints.push_back(std::move(curve));
        }
    }
    // Constraints (IK) follow; not used yet.
    return a;
}

void Animation::bind(const Skeleton& skeleton)
{
    for(JointCurve& curve : joints)
    {
        curve.joint = skeleton.find(curve.name);
    }
}

glm::quat sampleRotation(const Animation::JointCurve& curve, float time)
{
    const auto [key, t] = bracket(curve.rotations, time);
    return t > 0.0f ? nlerp(t, key->value, (key + 1)->value) : key->value;
}

glm::vec3 samplePosition(const Animation::JointCurve& curve, float time)
{
    const auto [key, t] = bracket(curve.positions, time);
    return t > 0.0f ? glm::mix(key->value, (key + 1)->value, t) : key->value;
}

void Animator::start(const Uuid& id, std::shared_ptr<const Animation> animation, double now)
{
    for(Playing& p : playing_)
    {
        if(p.id == id)
        {
            if(p.stop >= 0.0)
            {
                p = {id, std::move(animation), now, -1.0, nextOrder_++};
            }
            return;
        }
    }
    playing_.push_back({id, std::move(animation), now, -1.0, nextOrder_++});
}

void Animator::stop(const Uuid& id, double now)
{
    for(Playing& p : playing_)
    {
        if(p.id == id && p.stop < 0.0)
        {
            p.stop = now;
        }
    }
}

bool Animator::isPlaying(const Uuid& id) const
{
    return std::ranges::any_of(playing_,
                               [&](const Playing& p) { return p.id == id && p.stop < 0.0; });
}

void Animator::evaluate(double now, size_t jointCount, std::vector<JointPose>& pose)
{
    pose.assign(jointCount, JointPose{});

    struct State
    {
        int priority;
        uint64_t order;
        float weight;
        const Animation::JointCurve* curve;
        float time;
    };
    std::vector<std::vector<State>> states(jointCount);

    for(Playing& p : playing_)
    {
        const Animation& a = *p.animation;
        const auto elapsed = static_cast<float>(now - p.start);

        // Animations that don't loop stop by themselves, early enough to
        // ease out by their end (llmotioncontroller.cpp).
        if(p.stop < 0.0 && !a.loop && a.duration > 0.0f &&
           elapsed > std::max(a.duration - a.easeOut, 0.0f))
        {
            p.stop = p.start + std::max(a.duration - a.easeOut, 0.0f);
        }

        float weight = a.easeIn > 0.0f ? cubicStep(elapsed / a.easeIn) : 1.0f;
        if(p.stop >= 0.0)
        {
            const auto sinceStop = static_cast<float>(now - p.stop);
            weight *= a.easeOut > 0.0f ? cubicStep(1.0f - sinceStop / a.easeOut) : 0.0f;
        }

        // Where in the animation we are (llkeyframemotion.cpp onUpdate).
        float time = std::max(elapsed, 0.0f);
        if(a.loop)
        {
            if(a.duration == 0.0f)
            {
                time = 0.0f;
            }
            else if(time > a.loopOut)
            {
                const float length = a.loopOut - a.loopIn;
                time = length > 0.0f ? a.loopIn + std::fmod(time - a.loopOut, length) : a.loopOut;
            }
        }
        time = std::min(time, a.duration);

        for(const Animation::JointCurve& curve : a.joints)
        {
            if(curve.joint >= 0 && static_cast<size_t>(curve.joint) < jointCount && weight > 0.0f)
            {
                states[static_cast<size_t>(curve.joint)].push_back(
                    {curve.priority, p.order, weight, &curve, time});
            }
        }
    }
    std::erase_if(playing_, [&](const Playing& p)
                  { return p.stop >= 0.0 && now - p.stop >= p.animation->easeOut; });

    // Per joint, as LLJointStateBlender: highest priority (then latest)
    // first; each later state only fills what's left of a total weight of 1.
    for(size_t j = 0; j < jointCount; j++)
    {
        auto& list = states[j];
        if(list.empty())
        {
            continue;
        }
        std::ranges::sort(
            list, [](const State& a, const State& b)
            { return a.priority != b.priority ? a.priority > b.priority : a.order > b.order; });
        float rotationWeight = 0.0f;
        float positionWeight = 0.0f;
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 position(0.0f);
        for(const State& s : list)
        {
            if(!s.curve->rotations.empty())
            {
                const glm::quat value = sampleRotation(*s.curve, s.time);
                if(rotationWeight > 0.0f)
                {
                    const float sum = std::min(1.0f, s.weight + rotationWeight);
                    rotation = nlerp(rotationWeight / sum, value, rotation);
                    rotationWeight = sum;
                }
                else
                {
                    rotation = value;
                    rotationWeight = s.weight;
                }
            }
            if(!s.curve->positions.empty())
            {
                const glm::vec3 value = samplePosition(*s.curve, s.time);
                if(positionWeight > 0.0f)
                {
                    const float sum = std::min(1.0f, s.weight + positionWeight);
                    position = glm::mix(value, position, positionWeight / sum);
                    positionWeight = sum;
                }
                else
                {
                    position = value;
                    positionWeight = s.weight;
                }
            }
        }
        if(rotationWeight > 0.0f)
        {
            pose[j].rotation = rotation;
        }
        if(positionWeight > 0.0f)
        {
            pose[j].position = position;
        }
    }

    // What no animation moves stays as the last one left it.
    held_.resize(jointCount);
    for(size_t j = 0; j < jointCount; j++)
    {
        JointPose& p = pose[j];
        JointPose& h = held_[j];
        p.rotation ? void(h.rotation = p.rotation) : void(p.rotation = h.rotation);
        p.position ? void(h.position = p.position) : void(p.position = h.position);
    }
}

} // namespace viewer::avatar
