#pragma once

// The VR radial menu: a tree of items (each an action or a submenu) kept as
// data, so what's in it is written in one place (App::radialMenu) and easy
// to change. Shown as slices round a middle: up to kSlices to a ring; in a
// submenu one of them goes back; a level with more pages (its last slice
// turns the page). The middle is where pointing rests, choosing nothing; it
// shows the submenu's name.
//
// It's pointed at by direction (a controller's stick, or where a ray meets
// it from its middle) and chosen with choose(): an action runs (and the
// menu closes, unless the item keeps it open), a submenu opens, the page
// turns, the submenu goes back.

#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace viewer
{

struct MenuItem
{
    std::string label;
    std::function<void()> action;   // an action; or
    std::vector<MenuItem> children; // a submenu (if there are any)
    // Optional: a label worked out when shown ("Fly" or "Land"), and
    // whether it can be chosen now (greyed out if not).
    std::function<std::string()> labelNow;
    std::function<bool()> enabled;
    bool keepOpen = false; // the menu stays open after the action (toggles)

    static MenuItem run(std::string label, std::function<void()> action)
    {
        MenuItem item;
        item.label = std::move(label);
        item.action = std::move(action);
        return item;
    }
    static MenuItem submenu(std::string label, std::vector<MenuItem> children)
    {
        MenuItem item;
        item.label = std::move(label);
        item.children = std::move(children);
        return item;
    }
    // Chained settings: MenuItem::run(...).when(...).named(...).staying().
    MenuItem&& when(std::function<bool()> f) &&
    {
        enabled = std::move(f);
        return std::move(*this);
    }
    MenuItem&& named(std::function<std::string()> f) &&
    {
        labelNow = std::move(f);
        return std::move(*this);
    }
    MenuItem&& staying() &&
    {
        keepOpen = true;
        return std::move(*this);
    }

    bool isSubmenu() const { return !children.empty(); }
    bool isEnabled() const { return !enabled || enabled(); }
    std::string shownLabel() const { return labelNow ? labelNow() : label; }
};

class RadialMenu
{
public:
    static constexpr int kSlices = 8;

    // What a place on the menu is.
    struct Slot
    {
        enum class Kind
        {
            Item,
            NextPage,
            Back, // out of a submenu
        };
        Kind kind = Kind::Item;
        const MenuItem* item = nullptr;
        std::string label;
        bool enabled = true;
    };

    void setRoot(MenuItem root);

    // Shown with its middle at `frame` (render::Panel's: facing +Z), at the
    // top level.
    void open(const glm::mat4& frame);
    void close();
    bool isOpen() const { return open_; }
    void moveTo(const glm::mat4& frame) { frame_ = frame; }
    const glm::mat4& frame() const { return frame_; }

    // Pointing: by a stick (x right, y up), its cursor as far out on the
    // menu as the stick's pushed: a slice if it's far enough out, else the
    // middle (nothing); or by a ray, where it meets the menu. The ray's
    // answer is whether it's on the menu (and where; the middle too), so it
    // isn't the world's. Either way a cursor's drawn where it points.
    void pointToward(glm::vec2 stick);
    std::optional<glm::vec3> pointRay(const glm::vec3& origin, const glm::vec3& direction);
    void clearPoint()
    {
        hovered_.reset();
        cursor_.reset();
    }
    std::optional<glm::vec2> cursor() const { return cursor_; } // panel pixels

    // Chooses what's pointed at (see the top); false if nothing was.
    bool choose();

    // The slices of the level shown (its page), clockwise from the top; the
    // middle's label (the submenu's name); which slice is pointed at.
    std::vector<Slot> slots() const;
    std::string title() const;
    std::optional<int> hovered() const { return hovered_; }
    void hover(std::optional<int> slot) { hovered_ = slot; }
    int page() const { return levels_.empty() ? 0 : levels_.back().page; }
    int pages() const;
    size_t depth() const { return levels_.size(); }

    render::Panel panel() const;

private:
    const std::vector<MenuItem>& shownItems() const;
    // The items a page holds, and whether the level has a Back slice.
    int perPage() const;
    bool hasBack() const { return levels_.size() > 1; }
    // Which slot a point on the panel (its pixels) is on (nothing in the
    // middle).
    std::optional<int> slotAt(glm::vec2 pixel) const;
    glm::vec2 slotCenter(int slot, int count) const;

    MenuItem root_;
    bool open_ = false;
    glm::mat4 frame_{1.0f};
    struct Level
    {
        const MenuItem* menu = nullptr;
        int page = 0;
    };
    std::vector<Level> levels_;
    std::optional<int> hovered_;
    std::optional<glm::vec2> cursor_;
};

} // namespace viewer
