#include "ViewerEffects.hpp"

#include "viewer/protocol/Messages.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace viewer
{

namespace
{

constexpr float kForever = std::numeric_limits<float>::max() / 4.0f; // MAX_TIMEOUT
constexpr double kMinMove = 0.05;                                    // meters
constexpr double kMinInterval = 1.0 / 4.0;                           // MAX_SENDS_PER_SEC

double distance(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                     (a[2] - b[2]) * (a[2] - b[2]));
}

} // namespace

float ViewerEffects::lookDuration(uint8_t type)
{
    // llhudeffectlookat.cpp's attentions: idle 3 s, auto listen 4,
    // free look 2, respond 4, hover 1, clear 0, the rest for ever.
    switch(type)
    {
    case 1:
        return 3.0f;
    case 2:
    case 4:
        return 4.0f;
    case 3:
        return 2.0f;
    case 5:
        return 1.0f;
    case kLookClear:
        return 0.0f;
    default:
        return kForever;
    }
}

float ViewerEffects::pointDuration(uint8_t type)
{
    return type == kPointClear ? 0.0f : kForever;
}

void ViewerEffects::update(const Send& send, const std::optional<Target>& lookAt,
                           const std::optional<Target>& pointAt, double seconds)
{
    update(send, look_, lookAt, seconds, &ViewerEffects::lookDuration);
    update(send, point_, pointAt, seconds, &ViewerEffects::pointDuration);
}

void ViewerEffects::update(const Send& send, Channel& channel, const std::optional<Target>& want,
                           double seconds, float (*duration)(uint8_t))
{
    session::Session::AttentionEffect effect;
    effect.id = channel.id;
    effect.effectType = channel.effectType;
    if(!want)
    {
        if(channel.sent)
        {
            // As the official clear: the effect reset to "none" (no target,
            // no position) and sent lasting no time. (Sent as the clear
            // type itself, receivers would look or point at the global
            // origin.)
            effect.targetType = 0;
            effect.duration = 0.0f;
            send(effect);
            channel.sent.reset();
            channel.sentAt = seconds;
        }
        return;
    }
    const float lasts = duration(want->type);
    const bool changed =
        !channel.sent || channel.sent->type != want->type || channel.sent->object != want->object;
    const bool moved = channel.sent &&
                       distance(channel.sent->position, want->position) > kMinMove &&
                       seconds - channel.sentAt > kMinInterval;
    // Before it runs out over there.
    const bool expiring = lasts < kForever && seconds - channel.sentAt > lasts * 0.75;
    if(!changed && !moved && !expiring)
    {
        return;
    }
    effect.targetType = want->type;
    effect.duration = lasts;
    effect.target = want->object;
    effect.position = want->position;
    send(effect);
    channel.sent = want;
    channel.sentAt = seconds;
}

std::optional<std::pair<Uuid, ViewerEffects::Target>>
ViewerEffects::unpack(std::span<const uint8_t> data)
{
    if(data.size() != 57)
    {
        return std::nullopt;
    }
    Uuid source;
    std::copy(data.begin(), data.begin() + 16, source.bytes.begin());
    Target t;
    std::copy(data.begin() + 16, data.begin() + 32, t.object.bytes.begin());
    for(size_t i = 0; i < 3; i++)
    {
        uint64_t bits = 0; // little-endian whatever the host is
        for(size_t b = 0; b < 8; b++)
        {
            bits |= static_cast<uint64_t>(data[32 + i * 8 + b]) << (8 * b);
        }
        t.position[i] = std::bit_cast<double>(bits);
    }
    t.type = data[56];
    return std::pair(source, t);
}

std::optional<bool> ViewerEffects::unpackReset(std::span<const uint8_t> data, const Uuid& source)
{
    Uuid target;
    if(data.size() != 17 || source.isNull())
    {
        return std::nullopt;
    }
    std::copy_n(data.begin(), target.bytes.size(), target.bytes.begin());
    if(!target.isNull() && target != source)
    {
        return std::nullopt;
    }
    return (data[16] & 1) != 0;
}

void ViewerEffects::receive(const protocol::CircuitEvent& event, const Uuid& self)
{
    const auto m = event.decode<protocol::msg::ViewerEffect>();
    if(!m)
    {
        return;
    }
    for(const auto& effect : m->Effect)
    {
        if(effect.Type == session::Session::kResetSkeletonEffect)
        {
            const auto animations = unpackReset(effect.TypeData, effect.AgentID);
            if(animations && effect.AgentID != self)
            {
                resets_.emplace_back(effect.AgentID, *animations);
            }
            continue;
        }
        const bool look = effect.Type == session::Session::kLookAtEffect;
        if(!look && effect.Type != session::Session::kPointAtEffect)
        {
            continue; // beams, spirals and such: not drawn
        }
        const auto unpacked = unpack(effect.TypeData);
        if(!unpacked || unpacked->first == self || unpacked->first.isNull() ||
           effect.ID == look_.id || effect.ID == point_.id)
        {
            continue;
        }
        const auto& [source, target] = *unpacked;
        Attention& a = received_[source];
        std::optional<Target>& slot = look ? a.look : a.point;
        if(target.type == 0)
        {
            slot.reset(); // none: cleared
        }
        else
        {
            slot = target;
        }
        if(!a.look && !a.point)
        {
            received_.erase(source);
        }
    }
}

} // namespace viewer
