#include "VrControls.hpp"

#include "PanelGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

// The menu opened in front of the eyes this far; the keyboard this far
// ahead and down (a little below the eyes, tilted up at them).
constexpr float kMenuDistance = 0.5f;
constexpr float kKeyboardAhead = 0.45f;
constexpr float kKeyboardBelow = 0.3f;
// The menu's hand's stick pushed further than this points at a slice; let
// back nearer than this, it chooses it.
constexpr float kStickOut = 0.5f;
constexpr float kStickBack = 0.35f;
// The page this far ahead of the eyes and down.
constexpr float kPageAhead = 0.7f;
constexpr float kPageBelow = 0.08f;
// A controller's stick pushed all the way on the page scrolls this many
// lines a second.
constexpr float kScrollLines = 14.0f;
// A ray on nothing is drawn this long.
constexpr float kRayLength = 5.0f;

glm::vec3 level(const glm::vec3& forward)
{
    const glm::vec3 flat(forward.x, forward.y, 0.0f);
    return glm::length(flat) > 1e-4f ? glm::normalize(flat) : glm::vec3(1.0f, 0.0f, 0.0f);
}

} // namespace

glm::mat4 VrControls::anchored(const glm::mat4& frame, const glm::vec3& openedAt) const
{
    glm::mat4 moved = frame;
    moved[3] += glm::vec4(last_.anchor - openedAt, 0.0f);
    return moved;
}

void VrControls::openKeyboard()
{
    const glm::vec3 at =
        last_.eye + level(last_.forward) * kKeyboardAhead - glm::vec3(0.0f, 0.0f, kKeyboardBelow);
    keyboardFrame_ = panelFacing(at, last_.eye);
    keyboardAnchor_ = last_.anchor;
    keyboard_.open(keyboardFrame_);
}

glm::mat4 VrControls::slotFrame(const Slot& slot) const
{
    return slot.standing ? slot.frame : anchored(slot.frame, slot.anchor);
}

void VrControls::leave(Slot& slot)
{
    if(slot.hovered && slot.page.point)
    {
        slot.page.point(std::nullopt);
    }
    if(slot.pressed && slot.page.button)
    {
        slot.page.button(false);
    }
    slot.hovered = false;
    slot.pressed.reset();
}

void VrControls::setPage(Page page)
{
    leave(pages_[0]);
    pages_[0].page = std::move(page);
}

void VrControls::openPage()
{
    const glm::vec3 at =
        last_.eye + level(last_.forward) * kPageAhead - glm::vec3(0.0f, 0.0f, kPageBelow);
    pages_[0].frame = panelFacing(at, last_.eye);
    pages_[0].anchor = last_.anchor;
    pages_[0].open = true;
}

void VrControls::openPageAt(glm::vec3 center, glm::vec3 facing, glm::vec3 anchor)
{
    pages_[0].frame = panelFacing(center, facing);
    pages_[0].anchor = anchor;
    pages_[0].open = true;
}

void VrControls::closePage()
{
    leave(pages_[0]);
    pages_[0].open = false;
}

void VrControls::setStandingPages(std::vector<StandingPage> pages)
{
    for(size_t i = 1; i < pages_.size(); i++)
    {
        leave(pages_[i]);
    }
    pages_.resize(1);
    for(StandingPage& p : pages)
    {
        Slot slot;
        slot.page = std::move(p.page);
        slot.open = true;
        slot.standing = true;
        slot.frame = panelFacing(p.center, p.facing);
        pages_.push_back(std::move(slot));
    }
    for(auto& on : onPage_)
    {
        if(on && on->slot >= pages_.size())
        {
            on.reset();
        }
    }
}

std::string VrControls::cursorPointed(int pointer) const
{
    const auto& on = onPage_[static_cast<size_t>(pointer)];
    if(!on)
    {
        return {};
    }
    const Slot& slot = pages_[on->slot];
    return slot.page.cursor ? slot.page.cursor() : std::string();
}

bool VrControls::scrollPointed(int pointer, glm::vec2 lines)
{
    const auto& on = onPage_[static_cast<size_t>(pointer)];
    if(!on)
    {
        return false;
    }
    Slot& slot = pages_[on->slot];
    if(slot.page.scroll)
    {
        slot.page.scroll(lines);
    }
    return true;
}

std::array<VrControls::Pointed, VrControls::kPointers> VrControls::update(const Input& input)
{
    const float dt = static_cast<float>(std::clamp(input.now - lastNow_, 0.0, 0.1));
    lastNow_ = input.now;
    last_ = input;
    std::array<Pointed, kPointers> out{};

    // The menu: a pointer's menu button held a moment opens it there (or
    // moves it there, if it's open); pressed and let go sooner, it closes
    // it.
    for(int p = 0; p < kPointers; p++)
    {
        const auto i = static_cast<size_t>(p);
        const Pointer& pointer = input.pointers[i];
        if(pointer.menu && !menuButton_[i])
        {
            menuDownAt_[i] = input.now;
        }
        else if(!pointer.menu && menuButton_[i])
        {
            if(menuDownAt_[i])
            {
                // A press, not a hold.
                if(menu_.isOpen())
                {
                    menu_.close();
                }
                else if(mainMenu_)
                {
                    mainMenu_();
                }
            }
            menuDownAt_[i].reset();
        }
        if(pointer.menu && menuDownAt_[i] && input.now - *menuDownAt_[i] >= kHoldToOpen)
        {
            menuDownAt_[i].reset(); // held: its letting go won't close it
            const glm::vec3 at =
                pointer.menuAt ? *pointer.menuAt : input.eye + input.forward * kMenuDistance;
            menuFrame_ = panelFacing(at, input.eye);
            menuAnchor_ = input.anchor;
            if(!menu_.isOpen())
            {
                menu_.open(menuFrame_);
                stickChoice_.reset();
            }
            menuPointer_ = p;
        }
        menuButton_[i] = pointer.menu;
    }
    if(menu_.isOpen() && menuPointer_)
    {
        // At the hand that opened it, facing the eyes; else where it
        // opened, moving with the avatar.
        const Pointer& holder = input.pointers[static_cast<size_t>(*menuPointer_)];
        if(holder.active && holder.menuAt)
        {
            menuFrame_ = panelFacing(*holder.menuAt, input.eye);
            menuAnchor_ = input.anchor;
        }
        menu_.moveTo(anchored(menuFrame_, menuAnchor_));
    }

    // What each pointer is on: the menu, then the keyboard. The hand the
    // menu's at points at it with its stick (its cursor in the middle while
    // the stick's let be), unless the other hand's ray is on it; pushed out
    // to a slice, the stick wins.
    if(keyboard_.isOpen())
    {
        keyboard_.moveTo(anchored(keyboardFrame_, keyboardAnchor_));
    }
    std::optional<glm::vec2> holderStick;
    if(menu_.isOpen() && menuPointer_)
    {
        const Pointer& holder = input.pointers[static_cast<size_t>(*menuPointer_)];
        if(holder.active && holder.menuAt)
        {
            holderStick = holder.stick;
        }
    }
    if(!holderStick)
    {
        stickChoice_.reset();
    }
    const bool stickPushed =
        holderStick && (glm::length(*holderStick) > kStickOut || stickChoice_.has_value());
    bool rayOnMenu = false;
    std::array<bool, kPointers> onMenu{};
    std::array<std::optional<OnPage>, kPointers> onPage{};
    for(int p = 0; p < kPointers; p++)
    {
        const Pointer& pointer = input.pointers[static_cast<size_t>(p)];
        Pointed& result = out[static_cast<size_t>(p)];
        if(!pointer.active)
        {
            keyboard_.clearPoint(p);
            continue;
        }
        // The hand holding the menu open at itself doesn't point at it.
        const bool holdsMenuAtHand = menuPointer_ == p && pointer.menuAt;
        if(menu_.isOpen() && !stickPushed && !holdsMenuAtHand && !rayOnMenu)
        {
            if(const auto at = menu_.pointRay(pointer.origin, pointer.direction))
            {
                result.onControls = true;
                result.at = at;
                rayOnMenu = true;
                onMenu[static_cast<size_t>(p)] = true;
                keyboard_.clearPoint(p);
                continue;
            }
        }
        if(const auto at = keyboard_.point(p, pointer.origin, pointer.direction))
        {
            result.onControls = true;
            result.at = at;
            continue;
        }
        // A page: the nearest it's on, or (its button held down on one)
        // anywhere in that one's plane, so dragging doesn't stop at the
        // edge.
        std::optional<PanelHit> best;
        for(size_t s = 0; s < pages_.size(); s++)
        {
            const Slot& slot = pages_[s];
            const bool holding = slot.pressed == p;
            if(!slot.open || (!holding && std::ranges::any_of(pages_, [&](const Slot& o)
                                                              { return o.pressed == p; })))
            {
                continue;
            }
            const auto hit = panelHit(slotFrame(slot), slot.page.size, slot.page.metersPerPixel,
                                      pointer.origin, pointer.direction, holding);
            if(hit && (!best || hit->distance < best->distance))
            {
                best = hit;
                onPage[static_cast<size_t>(p)] = OnPage{s, hit->pixel};
            }
        }
        if(best)
        {
            result.onControls = true;
            result.at = best->position;
        }
    }
    // Which pointer each page follows: the one holding its button down,
    // else the right one on it, else the left.
    for(size_t s = 0; s < pages_.size(); s++)
    {
        Slot& slot = pages_[s];
        std::optional<int> driver = slot.pressed;
        for(int p = kPointers - 1; p >= 0 && !driver; p--)
        {
            const auto& on = onPage[static_cast<size_t>(p)];
            if(on && on->slot == s)
            {
                driver = p;
            }
        }
        const auto& on = driver ? onPage[static_cast<size_t>(*driver)] : std::nullopt;
        if(on && on->slot == s)
        {
            if(slot.page.point)
            {
                slot.page.point(on->pixel);
            }
            slot.hovered = true;
            // A controller's stick scrolls (not the desktop's: its wheel
            // does).
            const Pointer& d = input.pointers[static_cast<size_t>(*driver)];
            if(d.menuAt && std::abs(d.stick.y) > 0.25f && slot.page.scroll)
            {
                slot.page.scroll(glm::vec2(0.0f, -d.stick.y * kScrollLines * dt));
            }
        }
        else if(slot.hovered)
        {
            if(slot.page.point)
            {
                slot.page.point(std::nullopt);
            }
            slot.hovered = false;
        }
    }
    if(holderStick && !rayOnMenu)
    {
        // Pushed out: the slice it's on is the one to choose; let go (back
        // near the middle): chosen. Between, it stays the one.
        const float pushed = glm::length(*holderStick);
        menu_.pointToward(*holderStick);
        if(pushed > kStickOut)
        {
            if(menu_.hovered())
            {
                stickChoice_ = menu_.hovered();
            }
        }
        else if(pushed < kStickBack && stickChoice_)
        {
            menu_.hover(stickChoice_);
            stickChoice_.reset();
            menu_.choose();
            if(menu_.isOpen())
            {
                menu_.pointToward(*holderStick); // its cursor
            }
        }
        else if(stickChoice_)
        {
            menu_.hover(stickChoice_);
        }
    }
    else if(menu_.isOpen() && !rayOnMenu)
    {
        menu_.clearPoint();
    }

    // Buttons: on the controls, they're the controls'; else the world's.
    for(int p = 0; p < kPointers; p++)
    {
        const auto i = static_cast<size_t>(p);
        const Pointer& pointer = input.pointers[i];
        Pointed& result = out[i];
        const bool down = pointer.button && !button_[i];
        const bool up = !pointer.button && button_[i];
        if(down)
        {
            // A ray on the menu chooses what it's on; a ray on the
            // keyboard presses its key.
            bool used = false;
            if(menu_.isOpen() && onMenu[i])
            {
                menu_.choose();
                used = true;
            }
            else if(onPage[i])
            {
                Slot& slot = pages_[onPage[i]->slot];
                if(slot.page.point)
                {
                    slot.page.point(onPage[i]->pixel);
                }
                if(slot.page.button)
                {
                    slot.page.button(true);
                }
                slot.pressed = p;
                slot.hovered = true;
                used = true;
            }
            else if(result.onControls)
            {
                keyboard_.press(p);
                used = true;
            }
            pressedOnControls_[i] = used;
            result.pressed = !used;
        }
        if(up)
        {
            for(Slot& slot : pages_)
            {
                if(slot.pressed == p)
                {
                    if(slot.page.button)
                    {
                        slot.page.button(false);
                    }
                    slot.pressed.reset();
                }
            }
            result.released = !pressedOnControls_[i];
            pressedOnControls_[i] = false;
        }
        button_[i] = pointer.button;
    }
    pointed_ = out;
    onPage_ = onPage;
    return out;
}

void VrControls::addTo(render::Scene& scene, bool rays,
                       const std::array<std::optional<glm::vec3>, kPointers>& worldHits) const
{
    for(const Slot& slot : pages_)
    {
        if(!slot.open)
        {
            continue;
        }
        render::Panel page;
        page.frame = slotFrame(slot);
        page.size = slot.page.size;
        page.metersPerPixel = slot.page.metersPerPixel;
        page.image = slot.page.image;
        scene.panels.push_back(page);
    }
    if(keyboard_.isOpen())
    {
        scene.panels.push_back(keyboard_.panel()); // over the page
    }
    if(menu_.isOpen())
    {
        scene.panels.push_back(menu_.panel()); // over the keyboard
    }
    if(!rays)
    {
        return;
    }
    for(int p = 0; p < kPointers; p++)
    {
        const auto i = static_cast<size_t>(p);
        const Pointer& pointer = last_.pointers[i];
        if(!pointer.active || (menuPointer_ == p && menu_.isOpen() && pointer.menuAt))
        {
            continue;
        }
        render::PointerLine line;
        line.from = pointer.origin;
        const std::optional<glm::vec3> end = pointed_[i].at ? pointed_[i].at : worldHits[i];
        line.to = end ? *end : pointer.origin + pointer.direction * kRayLength;
        line.color = pointed_[i].onControls ? glm::vec4(0.55f, 0.75f, 1.0f, 0.9f)
                                            : glm::vec4(1.0f, 1.0f, 1.0f, end ? 0.6f : 0.3f);
        line.pixels = 3.0f;
        line.dot = end ? 10.0f : 0.0f;
        scene.pointerLines.push_back(line);
    }
}

} // namespace viewer
