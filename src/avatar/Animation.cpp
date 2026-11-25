#include "viewer/avatar/Animation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>

namespace viewer::avatar
{

namespace
{

constexpr uint16_t kVersion = 1;
constexpr uint16_t kSubVersion = 0;
constexpr int kHighestBasePriority = 6; // below LLJoint::ADDITIVE_PRIORITY
constexpr float kMaxPelvisOffset = 5.0f;
constexpr float kMaxDuration = 60.0f;
constexpr uint32_t kMaxJoints = 216;
constexpr int32_t kMaxConstraints = 10;
constexpr double kHandBlendSeconds = 0.2;

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
    a.basePriority = std::clamp(static_cast<int>(priority), -1, kHighestBasePriority);
    a.loop = loop != 0;
    // Only joints' own priorities count (LLKeyframeMotion's mMaxPriority).
    a.handPriority = priority > kHighestBasePriority ? kHighestBasePriority : 0;

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
        curve.priority = jointPriority < 0 ? a.basePriority : static_cast<int>(jointPriority);
        a.handPriority = std::max(a.handPriority, static_cast<int>(jointPriority));
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
    // Too many are ignored, as the official viewer does; ones that can't be
    // read too (it refuses the animation).
    int32_t constraints = 0;
    if(!r.read(constraints) || constraints < 0 || constraints > kMaxConstraints)
    {
        return a;
    }
    for(int32_t i = 0; i < constraints; i++)
    {
        Animation::Constraint c;
        uint8_t chain = 0;
        uint8_t type = 0;
        std::array<char, 16> source{};
        std::array<char, 16> target{};
        glm::vec3 direction(0.0f); // unused
        if(!r.read(chain) || !r.read(type) || !r.read(source) || !r.read(c.sourceOffset) ||
           !r.read(target) || !r.read(c.targetOffset) || !r.read(direction) ||
           !r.read(c.easeInStart) || !r.read(c.easeInStop) || !r.read(c.easeOutStart) ||
           !r.read(c.easeOutStop))
        {
            break;
        }
        auto finite = [](const glm::vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if(type > 1 || !finite(c.sourceOffset) || !finite(c.targetOffset) ||
           !std::isfinite(c.easeInStart) || !std::isfinite(c.easeInStop) ||
           !std::isfinite(c.easeOutStart) || !std::isfinite(c.easeOutStop))
        {
            continue;
        }
        auto text = [](const std::array<char, 16>& name)
        { return std::string(name.begin(), std::ranges::find(name, '\0')); };
        c.chainLength = chain;
        c.plane = type == 1;
        c.source = text(source);
        c.target = text(target);
        c.ground = c.target == "GROUND";
        a.constraints.push_back(std::move(c));
    }
    return a;
}

void Animation::bind(const Skeleton& skeleton)
{
    for(JointCurve& curve : joints)
    {
        curve.joint = skeleton.find(curve.name);
    }
    for(Constraint& c : constraints)
    {
        auto volume = [&](const std::string& name)
        {
            const int joint = skeleton.find(name);
            return joint >= 0 && skeleton.joints[static_cast<size_t>(joint)].volume ? joint : -1;
        };
        c.chain.clear();
        c.sourceJoint = volume(c.source);
        c.targetJoint = c.ground ? -1 : volume(c.target);
        if(c.sourceJoint < 0 || (!c.ground && c.targetJoint < 0))
        {
            continue;
        }
        // From the joint the volume is on up, each animated.
        int joint = c.sourceJoint;
        for(int i = 0; i <= c.chainLength; i++)
        {
            joint = skeleton.joints[static_cast<size_t>(joint)].parent;
            if(joint < 0 ||
               std::ranges::none_of(
                   joints, [&](const JointCurve& curve) { return curve.joint == joint; }))
            {
                c.chain.clear();
                break;
            }
            c.chain.push_back(joint);
        }
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

namespace
{

// llanimationstates.cpp's emotes and their avatar_lad.xml parameters.
struct Emote
{
    const char* id;
    const char* name;
    int param;
};
// clang-format off
constexpr Emote kEmotes[] = {
    {"aa2df84d-cf8f-7218-527b-424a52de766e", "express_afraid_emote", 310},
    {"1a03b575-9634-b62a-5767-3a679e81f4de", "express_anger_emote", 314},
    {"214aa6c1-ba6a-4578-f27c-ce7688f61d0d", "express_bored_emote", 307},
    {"d535471b-85bf-3b4d-a542-93bea4f59d33", "express_cry_emote", 312},
    {"d4416ff1-09d3-300f-4183-1b68a19b9fc1", "express_disdain", 309},
    {"0b8c8211-d78c-33e8-fa28-c51a9594e424", "express_embarrassed_emote", 304},
    {"fee3df48-fa3d-1015-1e26-a205810e3001", "express_frown", 315},
    {"1e8d90cc-a84e-e135-884c-7c82c8b03a14", "express_kiss", 306},
    {"62570842-0950-96f8-341c-809e65110823", "express_laugh_emote", 316},
    {"d63bc1f9-fc81-9625-a0c6-007176d82eb7", "express_open_mouth", 632},
    {"f76cda94-41d4-a229-2872-e0296e58afe1", "express_repulsed_emote", 308},
    {"eb6ebfb2-a4b3-a19c-d388-4dd5c03823f7", "express_sad_emote", 313},
    {"a351b1bc-cc94-aac2-7bea-a7e6ebad15ef", "express_shrug_emote", 305},
    {"b7c7c833-e3d3-c4e3-9fc0-131237446312", "express_smile", 318},
    {"728646d9-cc79-08b2-32d6-937f0a835c24", "express_surprise_emote", 302},
    {"835965c6-7f2f-bda2-5deb-2478737f91bf", "express_tongue_out", 301},
    {"b92ec1a5-e7ce-a76b-2b05-bcdb9311417e", "express_toothsmile", 317},
    {"da020525-4d94-59d6-23d7-81fdebf33148", "express_wink_emote", 303},
    {"9c05e5c7-6f07-6ca4-ed5a-b230390c3950", "express_worry_emote", 311},
};
// clang-format on

// By LLHandMotion::eHandPose: the parameter (spread hands have none).
constexpr std::array<int, 14> kHandParams{
    0, kRelaxedHandsParam, 102, 103, 666, 667, 668, 669, 670, 671, 766, 672, 791, 792};
constexpr uint32_t kRelaxedHands = 1;

struct EmoteAnimations
{
    std::map<Uuid, std::shared_ptr<const Animation>> byId;
    std::map<std::string, std::shared_ptr<const Animation>, std::less<>> byName;
    std::map<int, Uuid> ids;

    EmoteAnimations()
    {
        for(const Emote& e : kEmotes)
        {
            Animation a;
            a.easeIn = 0.3f; // llemote.h
            a.easeOut = 1.4f;
            a.duration = a.easeIn + 1.1f + a.easeOut;
            a.emoteParam = e.param;
            const auto shared = std::make_shared<const Animation>(std::move(a));
            const Uuid id = *Uuid::parse(e.id);
            byId[id] = shared;
            byName[e.name] = shared;
            ids[e.param] = id;
        }
    }
};

const EmoteAnimations& emoteAnimations()
{
    static const EmoteAnimations emotes;
    return emotes;
}

Uuid emoteId(int param)
{
    return emoteAnimations().ids.at(param);
}

} // namespace

std::shared_ptr<const Animation> emoteAnimation(const Uuid& id)
{
    const auto& emotes = emoteAnimations().byId;
    const auto it = emotes.find(id);
    return it != emotes.end() ? it->second : nullptr;
}

std::shared_ptr<const Animation> emoteAnimation(std::string_view name)
{
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& emotes = emoteAnimations().byName;
    const auto it = emotes.find(lower);
    return it != emotes.end() ? it->second : nullptr;
}

// Animations that don't loop stop by themselves, early enough to ease out by
// their end (llmotioncontroller.cpp).
bool Animator::stopAtEnd(Playing& p, double now)
{
    const Animation& a = *p.animation;
    if(p.stopped() || a.loop || a.duration <= 0.0f)
    {
        return false;
    }
    const double end = p.start + static_cast<double>(std::max(a.duration - a.easeOut, 0.0f));
    if(now <= end)
    {
        return false;
    }
    p.told = p.stop = end;
    p.ended = true;
    return true;
}

float Animator::weightAt(const Playing& p, double now)
{
    const Animation& a = *p.animation;
    // Easing out starts from the weight it had got to.
    const double until = p.stopped() ? std::min(now, p.stop) : now;
    const auto in = static_cast<float>(until - p.start);
    float weight = a.easeIn > 0.0f ? cubicStep(in / a.easeIn) : 1.0f;
    if(p.ended && p.stop <= p.start)
    {
        weight = 1.0f; // no longer than its ease out: all of it eases out
    }
    if(p.stopped() && now > p.stop)
    {
        weight *= a.easeOut > 0.0f ? cubicStep(1.0f - static_cast<float>(now - p.stop) / a.easeOut)
                                   : 0.0f;
    }
    return weight;
}

// Where in the animation it is (llkeyframemotion.cpp onUpdate).
float Animator::timeAt(const Playing& p, double now)
{
    const Animation& a = *p.animation;
    if(a.loop && a.duration == 0.0f)
    {
        return 0.0f;
    }
    auto looped = [&](double at)
    {
        const auto time = static_cast<float>(std::max(at - p.start, 0.0));
        if(!a.loop || time <= a.loopOut)
        {
            return time;
        }
        const float length = a.loopOut - a.loopIn;
        return length != 0.0f ? a.loopIn + std::fmod(time - a.loopOut, length) : a.loopOut;
    };
    // Once told to stop it no longer loops: it plays on to its end.
    const float time = p.stopped() && now > p.told
                           ? looped(p.told) + static_cast<float>(now - p.told)
                           : looped(now);
    return std::min(time, a.duration);
}

void Animator::start(const Uuid& id, std::shared_ptr<const Animation> animation, double now)
{
    for(Playing& p : playing_)
    {
        if(p.id != id || p.stopped() || stopAtEnd(p, now))
        {
            continue;
        }
        if(weightAt(p, now) >= 1.0f)
        {
            if(p.animation->emoteParam != 0)
            {
                // Emotes last their whole time again.
                p.start = now - static_cast<double>(p.animation->easeIn);
            }
            return; // keeps playing
        }
        stopPlaying(p, now);
    }
    const std::shared_ptr<const Animation> emote = emoteAnimation(animation->emote);
    playing_.push_back({id, std::move(animation), now, nextOrder_++});
    // Its emote (LLKeyframeMotion::onActivate), unless it shows already.
    if(emote)
    {
        // Emotes are identified by their parameter here.
        for(const Playing& p : playing_)
        {
            if(p.animation->emoteParam == emote->emoteParam)
            {
                return;
            }
        }
        playing_.push_back({emoteId(emote->emoteParam), emote, now, nextOrder_++});
    }
}

// As LLKeyframeMotion::setStopTime: one with more after its loop plays that
// before easing out.
void Animator::stopPlaying(Playing& p, double now)
{
    if(p.stopped())
    {
        return;
    }
    const Animation& a = *p.animation;
    p.told = p.stop = now;
    if(a.loop && a.loopOut != a.duration)
    {
        const double length = static_cast<double>(a.loopOut) - static_cast<double>(a.loopIn);
        const double into =
            length != 0.0 ? std::fmod(now - p.start - static_cast<double>(a.loopIn), length) : 0.0;
        p.stop = std::max(now, now - into + static_cast<double>(a.duration) -
                                   static_cast<double>(a.loopIn) - static_cast<double>(a.easeOut));
    }
}

void Animator::stop(const Uuid& id, double now)
{
    for(Playing& p : playing_)
    {
        if(p.id == id)
        {
            stopPlaying(p, now);
        }
    }
}

void Animator::stopAll(std::span<const Uuid> keep)
{
    std::erase_if(playing_,
                  [&](const Playing& p) { return std::ranges::find(keep, p.id) == keep.end(); });
}

bool Animator::isPlaying(const Uuid& id) const
{
    return std::ranges::any_of(playing_,
                               [&](const Playing& p) { return p.id == id && !p.stopped(); });
}

void Animator::evaluate(double now, size_t jointCount, std::vector<JointPose>& pose,
                        const ConstraintContext* context)
{
    ended_.clear();
    struct Current
    {
        float weight;
        float time;
        float seconds; // since it was last evaluated
    };
    std::vector<Current> current;
    current.reserve(playing_.size());
    States states(jointCount);

    for(Playing& p : playing_)
    {
        const Animation& a = *p.animation;
        if(stopAtEnd(p, now))
        {
            ended_.push_back(p.id);
        }
        float weight = weightAt(p, now);
        float time = timeAt(p, now);
        if(p.stopped() && now > p.stop + static_cast<double>(a.easeOut))
        {
            // Over since the last evaluation: its last moment shows once (it
            // may be what a joint is left with).
            p.done = p.updated ? *p.updated > p.stop : !p.ended;
            if(!p.done)
            {
                weight = p.ended ? 1.0f : weightAt(p, p.stop);
                time = timeAt(p, p.stop);
            }
            else
            {
                weight = 0.0f;
            }
        }
        current.push_back({weight, time, static_cast<float>(now - p.updated.value_or(now))});
        p.updated = now;

        for(const Animation::JointCurve& curve : a.joints)
        {
            if(curve.joint >= 0 && static_cast<size_t>(curve.joint) < jointCount && weight > 0.0f)
            {
                State s{&p, curve.priority, p.order, weight, {}, {}};
                if(!curve.rotations.empty())
                {
                    s.rotation = sampleRotation(curve, time);
                }
                if(!curve.positions.empty())
                {
                    s.position = samplePosition(curve, time);
                }
                states[static_cast<size_t>(curve.joint)].push_back(s);
            }
        }
    }

    // Per joint, as LLJointStateBlender: highest priority (then latest) first.
    claims_.assign(jointCount, 0.0f);
    for(size_t j = 0; j < jointCount; j++)
    {
        for(const State& s : states[j])
        {
            if(s.priority >= kLookAtPriority && s.rotation)
            {
                claims_[j] = std::min(1.0f, claims_[j] + s.weight);
            }
        }
        std::ranges::sort(
            states[j], [](const State& a, const State& b)
            { return a.priority != b.priority ? a.priority > b.priority : a.order > b.order; });
    }

    // Newest first, as the official viewer's updateMotionsByType: what newer
    // animations hold of every joint of an older one leaves it nothing to
    // say; constraints don't apply where they hold a chain's joint.
    struct Active
    {
        size_t playing;
        size_t constraint;
    };
    std::vector<Active> constraints;
    std::vector<int> heldRotation(jointCount, -1);
    std::vector<int> heldPosition(jointCount, -1);
    uint32_t hand = kRelaxedHands;
    int handPriority = -1;
    std::map<int, float> emotes;
    float mouth = 0.0f;
    for(size_t i = playing_.size(); i-- > 0;)
    {
        const Animation& a = *playing_[i].animation;
        const float weight = current[i].weight;
        if(playing_[i].done)
        {
            continue;
        }
        if(a.emoteParam != 0)
        {
            float& shown = emotes[a.emoteParam];
            shown = std::max(shown, weight);
            mouth = std::max(mouth, weight);
            continue;
        }
        std::vector<Active> its;
        for(size_t c = 0; c < a.constraints.size(); c++)
        {
            const auto& chain = a.constraints[c].chain;
            if(!chain.empty() &&
               std::ranges::none_of(chain,
                                    [&](int j)
                                    {
                                        return static_cast<size_t>(j) >= jointCount ||
                                               heldRotation[static_cast<size_t>(j)] >=
                                                   a.basePriority;
                                    }))
            {
                its.push_back({i, c});
            }
        }
        bool updated = weight < 1.0f;
        if(!updated)
        {
            for(const Animation::JointCurve& curve : a.joints)
            {
                if(curve.joint < 0 || static_cast<size_t>(curve.joint) >= jointCount)
                {
                    continue;
                }
                const auto j = static_cast<size_t>(curve.joint);
                auto hold = [&](std::vector<int>& held, bool moves)
                {
                    if(moves && curve.priority > held[j])
                    {
                        held[j] = curve.priority;
                        updated = true;
                    }
                };
                hold(heldRotation, !curve.rotations.empty());
                hold(heldPosition, !curve.positions.empty());
            }
        }
        if(!updated)
        {
            continue;
        }
        if(a.handPose < kHandParams.size() && a.handPriority >= handPriority)
        {
            hand = a.handPose;
            handPriority = a.handPriority;
        }
        if(weight > 0.0f)
        {
            constraints.insert(constraints.end(), its.begin(), its.end());
        }
    }

    pose.assign(jointCount, JointPose{});
    blend(states, pose);
    if(context && !constraints.empty() && jointCount == context->base.skeleton.joints.size())
    {
        // On the pose without them: first those that move the pelvis (the
        // rest reach from it), then to the ground, then to other parts
        // (which those may have moved).
        std::ranges::stable_sort(constraints, {},
                                 [&](const Active& c)
                                 {
                                     const auto& its =
                                         playing_[c.playing].animation->constraints[c.constraint];
                                     return its.chainLength == 0 ? 0 : its.ground ? 1 : 2;
                                 });
        for(const Active& c : constraints)
        {
            std::vector<JointPose> whole(jointCount);
            blend(states, whole);
            if(context->complete)
            {
                context->complete(whole);
            }
            constrain(playing_[c.playing], c.constraint, current[c.playing].time,
                      current[c.playing].seconds, *context, whole, states);
        }
        pose.assign(jointCount, JointPose{});
        blend(states, pose);
    }

    // What no animation moves stays as the last one left it.
    held_.resize(jointCount);
    for(size_t j = 0; j < jointCount; j++)
    {
        held_[j] = pose[j];
    }
    express(now, hand, mouth, emotes);
    std::erase_if(playing_, [](const Playing& p) { return p.done; });
}

// Hand poses fade into each other (llhandmotion.cpp); emotes follow their
// weights, against the closed mouth (llemote.cpp).
void Animator::express(double now, uint32_t hand, float mouth, const std::map<int, float>& emotes)
{
    const float step =
        expressed_
            ? static_cast<float>(std::clamp((now - *expressed_) / kHandBlendSeconds, 0.0, 1.0))
            : 1.0f;
    expressed_ = now;
    std::map<int, float> next = emotes;
    next[kClosedMouthParam] = 1.0f - mouth;
    for(size_t i = 1; i < kHandParams.size(); i++)
    {
        const auto was = expression_.find(kHandParams[i]);
        const float from = was != expression_.end() ? was->second : 0.0f;
        const float weight = std::clamp(i == hand ? from + step : from - step, 0.0f, 1.0f);
        if(weight > 0.0f)
        {
            next[kHandParams[i]] = weight;
        }
    }
    expression_ = std::move(next);
}

// Each later state only fills what's left of a total weight of 1. What none
// moves is as held.
void Animator::blend(const States& states, std::vector<JointPose>& pose) const
{
    for(size_t j = 0; j < pose.size(); j++)
    {
        float rotationWeight = 0.0f;
        float positionWeight = 0.0f;
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 position(0.0f);
        for(const State& s : states[j])
        {
            if(s.rotation)
            {
                if(rotationWeight > 0.0f)
                {
                    const float sum = std::min(1.0f, s.weight + rotationWeight);
                    rotation = nlerp(rotationWeight / sum, *s.rotation, rotation);
                    rotationWeight = sum;
                }
                else
                {
                    rotation = *s.rotation;
                    rotationWeight = s.weight;
                }
            }
            if(s.position)
            {
                if(positionWeight > 0.0f)
                {
                    const float sum = std::min(1.0f, s.weight + positionWeight);
                    position = glm::mix(*s.position, position, positionWeight / sum);
                    positionWeight = sum;
                }
                else
                {
                    position = *s.position;
                    positionWeight = s.weight;
                }
            }
        }
        if(j < held_.size())
        {
            pose[j] = held_[j];
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
}

} // namespace viewer::avatar
