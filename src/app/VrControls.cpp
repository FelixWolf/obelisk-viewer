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

void VrControls::openPage()
{
    const glm::vec3 at =
        last_.eye + level(last_.forward) * kPageAhead - glm::vec3(0.0f, 0.0f, kPageBelow);
    pageFrame_ = panelFacing(at, last_.eye);
    pageAnchor_ = last_.anchor;
    pageOpen_ = true;
}

void VrControls::closePage()
{
    if(pageHovered_ && page_.point)
    {
        page_.point(std::nullopt);
    }
    if(pagePressed_ && page_.button)
    {
        page_.button(false);
    }
    pageHovered_ = false;
    pagePressed_.reset();
    pageOpen_ = false;
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
    const glm::mat4 pageFrame = anchored(pageFrame_, pageAnchor_);
    std::array<std::optional<glm::vec2>, kPointers> onPage{};
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
        // The page: on it, or (its button held down on it) anywhere in its
        // plane, so dragging doesn't stop at the edge.
        if(pageOpen_)
        {
            if(const auto hit = panelHit(pageFrame, page_.size, page_.metersPerPixel,
                                         pointer.origin, pointer.direction, pagePressed_ == p))
            {
                result.onControls = true;
                result.at = hit->position;
                onPage[static_cast<size_t>(p)] = hit->pixel;
            }
        }
    }
    // Which pointer the page follows: the one holding its button down, else
    // the right one on it, else the left.
    std::optional<int> pageDriver = pagePressed_;
    for(int p = kPointers - 1; p >= 0 && !pageDriver; p--)
    {
        if(onPage[static_cast<size_t>(p)])
        {
            pageDriver = p;
        }
    }
    if(pageDriver && onPage[static_cast<size_t>(*pageDriver)])
    {
        if(page_.point)
        {
            page_.point(onPage[static_cast<size_t>(*pageDriver)]);
        }
        pageHovered_ = true;
        // A controller's stick scrolls (not the desktop's: its wheel does).
        const Pointer& driver = input.pointers[static_cast<size_t>(*pageDriver)];
        if(driver.menuAt && std::abs(driver.stick.y) > 0.25f && page_.scroll)
        {
            page_.scroll(glm::vec2(0.0f, -driver.stick.y * kScrollLines * dt));
        }
    }
    else if(pageHovered_)
    {
        if(page_.point)
        {
            page_.point(std::nullopt);
        }
        pageHovered_ = false;
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
                if(page_.point)
                {
                    page_.point(onPage[i]);
                }
                if(page_.button)
                {
                    page_.button(true);
                }
                pagePressed_ = p;
                pageHovered_ = true;
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
            if(pagePressed_ == p)
            {
                if(page_.button)
                {
                    page_.button(false);
                }
                pagePressed_.reset();
            }
            result.released = !pressedOnControls_[i];
            pressedOnControls_[i] = false;
        }
        button_[i] = pointer.button;
    }
    pointed_ = out;
    return out;
}

void VrControls::addTo(render::Scene& scene, bool rays,
                       const std::array<std::optional<glm::vec3>, kPointers>& worldHits) const
{
    if(pageOpen_)
    {
        render::Panel page;
        page.frame = anchored(pageFrame_, pageAnchor_);
        page.size = page_.size;
        page.metersPerPixel = page_.metersPerPixel;
        page.image = page_.image;
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
