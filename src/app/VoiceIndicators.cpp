#include "VoiceIndicators.hpp"

#include <algorithm>

namespace viewer
{

namespace
{

// llvoicevisualizer.cpp's GREEN_THRESHOLD (and the least that makes waves),
// the most, OVERDRIVEN_POWER_LEVEL, BASE_BRIGHTNESS, FADE_OUT_DURATION.
constexpr float kLeast = 0.2f;
constexpr float kMost = 0.7f;
constexpr float kOverdriven = 0.7f;
constexpr float kGray = 0.7f;
constexpr float kFadeSeconds = 0.4f;
constexpr glm::vec4 kMuted{0.85f, 0.35f, 0.35f, 0.7f};
constexpr float kDotAlpha = 0.85f;

} // namespace

float VoiceIndicators::level(float power)
{
    return std::clamp((power - kLeast) / (kMost - kLeast), 0.0f, 1.0f);
}

glm::vec3 VoiceIndicators::color(float power)
{
    if(power >= kOverdriven)
    {
        return {1.0f, 0.2f, 0.2f};
    }
    return glm::mix(glm::vec3(kGray), glm::vec3(0.2f, 1.0f, 0.2f), level(power));
}

void VoiceIndicators::beginFrame(float seconds)
{
    seconds_ = seconds;
    std::erase_if(people_, [](const auto& p) { return !p.second.seen; });
    for(auto& [id, p] : people_)
    {
        p.seen = false;
    }
}

std::optional<render::VoiceMark>
VoiceIndicators::get(const Uuid& id, const std::optional<voice::VoiceClient::Status>& status)
{
    if(!status)
    {
        return std::nullopt;
    }
    Person& p = people_[id];
    p.seen = true;
    if(status->moderatorMuted)
    {
        p.power = 0.0f;
        return render::VoiceMark{0.0f, kMuted};
    }
    const float fallen = p.power - seconds_ * kMost / kFadeSeconds;
    p.power = std::max(status->speaking ? status->power : 0.0f, std::max(fallen, 0.0f));
    return render::VoiceMark{level(p.power), glm::vec4(color(p.power), kDotAlpha)};
}

} // namespace viewer
