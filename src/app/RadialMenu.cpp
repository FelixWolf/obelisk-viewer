#include "RadialMenu.hpp"

#include "PanelGeometry.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace viewer
{

namespace
{

// The layout, in panel pixels: slices round a ring, a round middle.
constexpr float kSize = 460.0f;
constexpr float kMetersPerPixel = 0.0007f; // about 32 cm across
constexpr float kRing = 160.0f;            // slices' centers from the middle
constexpr glm::vec2 kSlice{132.0f, 60.0f};
constexpr float kSliceCorner = 14.0f;
constexpr float kMiddle = 46.0f; // the middle's radius
constexpr float kOuter = 230.0f; // further out than this isn't on the menu
constexpr float kTextPixels = 22.0f;
// A stick pushed less far than this points at the middle.
constexpr float kStickDeadZone = 0.5f;
constexpr float kCursor = 9.0f; // its radius
const glm::vec4 kCursorColor{1.0f, 1.0f, 1.0f, 0.95f};

const glm::vec4 kSliceColor{0.08f, 0.10f, 0.14f, 0.85f};
const glm::vec4 kHoverColor{0.22f, 0.45f, 0.85f, 0.95f};
const glm::vec4 kMiddleColor{0.16f, 0.18f, 0.24f, 0.9f};
const glm::vec4 kTextColor{1.0f, 1.0f, 1.0f, 1.0f};
const glm::vec4 kDisabledText{0.55f, 0.55f, 0.6f, 1.0f};

} // namespace

void RadialMenu::setRoot(MenuItem root)
{
    root_ = std::move(root);
    levels_.clear();
    hovered_.reset();
    open_ = false;
}

void RadialMenu::open(const glm::mat4& frame)
{
    frame_ = frame;
    levels_ = {Level{&root_, 0}};
    hovered_.reset();
    open_ = true;
}

void RadialMenu::close()
{
    open_ = false;
    levels_.clear();
    clearPoint();
}

const std::vector<MenuItem>& RadialMenu::shownItems() const
{
    static const std::vector<MenuItem> kNone;
    return levels_.empty() ? kNone : levels_.back().menu->children;
}

int RadialMenu::perPage() const
{
    const int count = static_cast<int>(shownItems().size());
    const int room = kSlices - (hasBack() ? 1 : 0);
    return count <= room ? room : room - 1; // the last slice turns the page
}

int RadialMenu::pages() const
{
    const int count = static_cast<int>(shownItems().size());
    const int per = perPage();
    return std::max(1, (count + per - 1) / per);
}

std::vector<RadialMenu::Slot> RadialMenu::slots() const
{
    std::vector<Slot> out;
    const std::vector<MenuItem>& items = shownItems();
    const int total = pages();
    const int first = page() * perPage();
    const int last = std::min(static_cast<int>(items.size()), first + perPage());
    for(int i = first; i < last; i++)
    {
        const MenuItem& item = items[static_cast<size_t>(i)];
        out.push_back({Slot::Kind::Item, &item,
                       item.isSubmenu() ? item.shownLabel() + " >" : item.shownLabel(),
                       item.isEnabled()});
    }
    if(total > 1)
    {
        out.push_back(
            {Slot::Kind::NextPage, nullptr, std::format("More {}/{}", page() + 1, total), true});
    }
    if(hasBack())
    {
        out.push_back({Slot::Kind::Back, nullptr, "< Back", true});
    }
    return out;
}

std::string RadialMenu::title() const
{
    return hasBack() ? levels_.back().menu->shownLabel() : std::string();
}

glm::vec2 RadialMenu::slotCenter(int slot, int count) const
{
    // Clockwise from the top (pixels: y down).
    const float angle = glm::half_pi<float>() -
                        glm::two_pi<float>() * static_cast<float>(slot) / static_cast<float>(count);
    return glm::vec2(kSize * 0.5f) + glm::vec2(std::cos(angle), -std::sin(angle)) * kRing;
}

std::optional<int> RadialMenu::slotAt(glm::vec2 pixel) const
{
    const glm::vec2 from = pixel - glm::vec2(kSize * 0.5f);
    const float away = glm::length(from);
    if(away <= kMiddle)
    {
        return std::nullopt;
    }
    if(away > kOuter)
    {
        return std::nullopt;
    }
    const int count = static_cast<int>(slots().size());
    if(count == 0)
    {
        return std::nullopt;
    }
    // Clockwise from the top, as slotCenter.
    const float angle = std::atan2(-from.y, from.x);
    const float turned = glm::half_pi<float>() - angle;
    const float step = glm::two_pi<float>() / static_cast<float>(count);
    const int slot = static_cast<int>(std::lround(turned / step));
    return ((slot % count) + count) % count;
}

void RadialMenu::pointToward(glm::vec2 stick)
{
    if(!open_)
    {
        clearPoint();
        return;
    }
    // Within the dead zone: across the middle; past it: out over the
    // slices to the ring (y up to pixels' y down).
    const float pushed = std::min(glm::length(stick), 1.0f);
    const float away =
        pushed < kStickDeadZone
            ? pushed / kStickDeadZone * kMiddle
            : kMiddle + (pushed - kStickDeadZone) / (1.0f - kStickDeadZone) * (kRing - kMiddle);
    const glm::vec2 toward =
        pushed > 1e-4f ? glm::vec2(stick.x, -stick.y) / glm::length(stick) : glm::vec2(0.0f);
    cursor_ = glm::vec2(kSize * 0.5f) + toward * away;
    hovered_ = slotAt(*cursor_);
}

std::optional<glm::vec3> RadialMenu::pointRay(const glm::vec3& origin, const glm::vec3& direction)
{
    if(!open_)
    {
        return std::nullopt;
    }
    const auto hit = panelHit(frame_, glm::vec2(kSize), kMetersPerPixel, origin, direction);
    if(!hit)
    {
        return std::nullopt;
    }
    if(glm::length(hit->pixel - glm::vec2(kSize * 0.5f)) > kOuter)
    {
        clearPoint();
        return std::nullopt; // in the panel's corners: not on the menu
    }
    hovered_ = slotAt(hit->pixel);
    cursor_ = hit->pixel;
    return hit->position;
}

bool RadialMenu::choose()
{
    if(!open_ || !hovered_)
    {
        return false;
    }
    const int chosen = *hovered_;
    hovered_.reset();
    const std::vector<Slot> shown = slots();
    if(chosen < 0 || chosen >= static_cast<int>(shown.size()))
    {
        return false;
    }
    const Slot& slot = shown[static_cast<size_t>(chosen)];
    if(slot.kind == Slot::Kind::Back)
    {
        levels_.pop_back();
        return true;
    }
    if(slot.kind == Slot::Kind::NextPage)
    {
        levels_.back().page = (levels_.back().page + 1) % pages();
        return true;
    }
    if(!slot.item || !slot.enabled)
    {
        return false;
    }
    if(slot.item->isSubmenu())
    {
        levels_.push_back({slot.item, 0});
        return true;
    }
    // The action may change what the menu holds: closed (if it closes)
    // before it runs.
    const std::function<void()> action = slot.item->action;
    if(!slot.item->keepOpen)
    {
        close();
    }
    if(action)
    {
        action();
    }
    return true;
}

render::Panel RadialMenu::panel() const
{
    render::Panel p;
    p.frame = frame_;
    p.size = glm::vec2(kSize);
    p.metersPerPixel = kMetersPerPixel;
    const std::vector<Slot> shown = slots();
    const int count = static_cast<int>(shown.size());
    for(int i = 0; i < count; i++)
    {
        const Slot& slot = shown[static_cast<size_t>(i)];
        const glm::vec2 center = slotCenter(i, count);
        const bool hot = hovered_ == i && slot.enabled;
        p.boxes.push_back(
            {center - kSlice * 0.5f, kSlice, hot ? kHoverColor : kSliceColor, kSliceCorner});
        p.texts.push_back({slot.label, center, kTextPixels,
                           slot.enabled ? kTextColor : kDisabledText, kSlice.x - 8.0f});
    }
    const glm::vec2 middlePoint(kSize * 0.5f);
    p.boxes.push_back(
        {middlePoint - glm::vec2(kMiddle), glm::vec2(kMiddle * 2.0f), kMiddleColor, kMiddle});
    const std::string name = title();
    if(!name.empty())
    {
        p.texts.push_back({name, middlePoint, kTextPixels * 0.8f, kDisabledText, kMiddle * 1.9f});
    }
    if(cursor_)
    {
        // Over the slices and their labels.
        p.over.push_back(
            {*cursor_ - glm::vec2(kCursor), glm::vec2(kCursor * 2.0f), kCursorColor, kCursor});
    }
    return p;
}

} // namespace viewer
