#include "NameTags.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace viewer
{

namespace
{

// Shown fully this near, gone this much further on (CHAT_NORMAL_RADIUS, 5);
// with chat, twice as far.
constexpr float kFadeDistance = 20.0f;
constexpr float kChatFadeDistance = 40.0f;
constexpr float kFadeRange = 5.0f;
// Above the head by this much of the avatar's height
// (NAMETAG_VERT_OFFSET_WEIGHT), then this many pixels higher on screen
// (NAMETAG_VERTICAL_SCREEN_OFFSET).
constexpr float kHeadOffset = 0.17f;
constexpr float kRaise = 25.0f;
// The bubble: NameTagBackground at ChatBubbleOpacity.
constexpr glm::vec4 kBubble{0.0f, 0.0f, 0.0f, 0.5f};
// NameTagMatch, NameTagMismatch: white; the username a little darker.
constexpr glm::vec4 kNameColor{1.0f};
constexpr float kUserNameShade = 0.83f;
// Chat: others' (AgentChatColor) and ours (UserChatColor), as new lines;
// older ones toward gray.
constexpr glm::vec4 kOthersChat{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kOwnChat{1.0f, 1.0f, 0.0f, 1.0f};
// SansSerif and SansSerifSmall.
constexpr float kNamePoints = 10.0f;
constexpr float kSmallPoints = 9.0f;

// Animations the simulator signals for states (llanimationstates.cpp).
const Uuid kAway = *Uuid::parse("fd037134-85d4-f241-72c6-4f42164fedee");
const Uuid kDoNotDisturb = *Uuid::parse("efcf670c-2d18-8128-973a-034ebc806b67");
const Uuid kCustomize = *Uuid::parse("038fcec9-5ebd-8a8e-0e2e-6e71a0a1ac53");

std::string statusLine(const world::World& world, const Uuid& avatar)
{
    const auto* playing = world.animations(avatar);
    if(!playing)
    {
        return {};
    }
    auto has = [&](const Uuid& id)
    {
        return std::any_of(playing->begin(), playing->end(),
                           [&](const world::PlayingAnimation& a) { return a.id == id; });
    };
    std::string line;
    auto add = [&](const char* text)
    {
        if(!line.empty())
        {
            line += ", ";
        }
        line += text;
    };
    if(has(kAway))
    {
        add("Away");
    }
    if(has(kDoNotDisturb))
    {
        add("Do Not Disturb");
    }
    if(has(kCustomize))
    {
        add("(Editing Appearance)");
    }
    return line;
}

// Control characters in a group title show as '?' (LL_UNKNOWN_CHAR).
std::string printable(std::string text)
{
    for(char& c : text)
    {
        if(static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            c = '?';
        }
    }
    return text;
}

// a to b as t goes from `from` to `to`.
glm::vec4 blend(const glm::vec4& a, const glm::vec4& b, float t, float from, float to)
{
    return glm::mix(a, b, std::clamp((t - from) / (to - from), 0.0f, 1.0f));
}

// The chat lines (and "..." while typing) for a bubble, faded by `alpha`.
void addChatLines(render::Label& label, const LocalChat::Bubble& bubble, bool self,
                  LocalChat::Clock::time_point now, float alpha)
{
    const glm::vec4 fresh = self ? kOwnChat : kOthersChat;
    const glm::vec4 normal = glm::mix(fresh, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), 0.7f);
    const glm::vec4 old = glm::mix(normal, glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), 0.7f);
    const float fade = std::chrono::duration<float>(LocalChat::kFadeTime).count();
    auto first = bubble.lines.begin();
    if(bubble.typing && bubble.lines.size() >= 12 && first != bubble.lines.end())
    {
        ++first; // room for the dots
    }
    for(auto it = first; it != bubble.lines.end(); ++it)
    {
        const float age = std::chrono::duration<float>(now - it->time).count() / fade;
        glm::vec4 color;
        if(age < 1.0f)
        {
            color = blend(fresh, normal, age, 0.9f, 1.0f);
        }
        else if(age < 2.0f)
        {
            color = blend(normal, old, age, 1.9f, 2.0f);
        }
        else if(age < 3.0f)
        {
            color = old;
        }
        else
        {
            continue;
        }
        color.a *= alpha;
        label.lines.push_back({it->text, color, kNamePoints});
    }
    if(bubble.typing)
    {
        const float seconds = std::chrono::duration<float>(now - bubble.typingSince).count();
        const int dots = (static_cast<int>(std::floor(seconds * 3.0f)) + 2) % 3 + 1;
        glm::vec4 color = fresh;
        color.a *= alpha;
        label.lines.push_back({std::string(static_cast<size_t>(dots), '.'), color, kNamePoints});
    }
}

} // namespace

void addNameTags(render::Scene& scene, const world::World& world, const AvatarView& avatars,
                 AvatarNames& names, LocalChat& chat, const Uuid& self, uint64_t originHandle,
                 LocalChat::Clock::time_point now)
{
    const render::Camera& camera = scene.camera;
    const glm::vec3 forward = glm::normalize(camera.target - camera.position);
    const int head = avatars.base().skeleton.find("mHead");
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Avatar ||
           (object.firstName.empty() && object.lastName.empty()))
        {
            continue;
        }
        const LocalChat::Bubble* bubble = chat.bubble(object.id, now);
        const float fadeDistance = bubble ? kChatFadeDistance : kFadeDistance;
        const glm::mat4 root = avatars.rootFrame(object, world, originHandle);
        const glm::vec3 rootPosition(root[3]);
        const auto& joints = avatars.posedJoints(object);
        const glm::vec3 headPosition =
            head >= 0 && static_cast<size_t>(head) < joints.size()
                ? glm::vec3(root * joints[static_cast<size_t>(head)][3])
                : rootPosition + glm::vec3(0.0f, 0.0f, object.scale.z * 0.4f);

        // Far or small on screen: faded (mAppAngle: the avatar's size over
        // its distance, full from 5 degrees, gone at 2).
        const glm::vec3 toAvatar = rootPosition - camera.position;
        const float range = glm::length(toAvatar);
        const float radius = glm::length(object.scale) * 0.5f;
        const float angle = range < 0.001f ? 180.0f : glm::degrees(std::atan2(radius, range));
        float alpha = angle > 5.0f ? 1.0f : std::max(0.0f, (angle - 2.0f) / 3.0f);
        if(alpha <= 0.0f)
        {
            continue;
        }

        // Over the head, toward the camera's up and the camera itself, on
        // an ellipsoid around the body (idleCalcNameTagPosition).
        const glm::vec3 view = range > 0.001f ? toAvatar / range : forward;
        const glm::vec3 left = glm::normalize(glm::cross(camera.up, forward));
        glm::vec3 up = glm::cross(view, left);
        up = glm::length(up) > 1e-4f ? glm::normalize(up) : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::mat3 rotation(root);
        const glm::vec3 ellipsoid(object.scale.x * 0.4f, object.scale.y * 0.4f,
                                  object.scale.z * kHeadOffset);
        const glm::vec3 upOffset = rotation * (glm::transpose(rotation) * up * ellipsoid);
        const glm::vec3 atOffset = rotation * (glm::transpose(rotation) * view * ellipsoid);
        render::Label label;
        label.style = render::Label::Style::NameTag;
        label.position = headPosition + upOffset - glm::dot(atOffset, view) * view;

        // Nearer the camera by the avatar's radius, but not past the near
        // plane (LLHUDNameTag::updateVisibility); faded with distance.
        const glm::vec3 offset = label.position - camera.position;
        const float along = glm::dot(offset, forward);
        if(along <= 0.0f)
        {
            continue;
        }
        const float nearest = camera.nearPlane + 0.1f;
        if(along <= nearest + radius)
        {
            label.position = camera.position + offset * (nearest / along);
        }
        else
        {
            label.position -= glm::normalize(offset) * radius;
        }
        const float distance = glm::length(label.position - camera.position);
        if(distance > fadeDistance + kFadeRange)
        {
            continue;
        }
        if(distance > fadeDistance)
        {
            alpha *= 1.0f - (distance - fadeDistance) / kFadeRange;
        }
        if(alpha < 0.01f)
        {
            continue;
        }

        // The names; with chat, black in the band at the top, all in the
        // tag's font (LLHUDNameTag labels).
        glm::vec4 color = bubble ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : kNameColor;
        color.a *= alpha;
        const bool header = bubble != nullptr;
        auto addName = [&](std::string text, const glm::vec4& c, float points)
        { label.lines.push_back({std::move(text), c, header ? kNamePoints : points, header}); };
        if(std::string status = statusLine(world, object.id); !status.empty())
        {
            addName(std::move(status), color, kSmallPoints);
        }
        if(!object.groupTitle.empty())
        {
            addName(printable(object.groupTitle), color, kSmallPoints);
        }
        if(const AvatarName* name = names.get(object.id))
        {
            addName(name->displayName, color, kNamePoints);
            if(!name->displayNameDefault)
            {
                glm::vec4 shade = color;
                shade.r *= kUserNameShade;
                shade.g *= kUserNameShade;
                shade.b *= kUserNameShade;
                addName(name->userName(), shade, kSmallPoints);
            }
        }
        else
        {
            AvatarName legacy;
            legacy.legacyFirstName = object.firstName;
            legacy.legacyLastName = object.lastName;
            addName(legacy.userName(), color, kNamePoints);
        }
        if(bubble)
        {
            addChatLines(label, *bubble, object.id == self, now, alpha);
            label.alignLeft = true;
            label.headerBackground =
                glm::vec4(kNameColor.r, kNameColor.g, kNameColor.b, kBubble.a * alpha);
        }
        label.background = kBubble;
        label.background.a *= alpha;
        label.raise = kRaise;
        scene.labels.push_back(std::move(label));
    }
}

} // namespace viewer
