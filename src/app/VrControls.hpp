#pragma once

// The VR controls: the radial menu, opened by holding the menu button a
// moment (at the hand that holds it, and following it) and closed by
// pressing it again (a press otherwise opens the main menu, the app's);
// chosen from by that hand's stick pushed to a slice and let go, or by a
// ray and its trigger. And the keyboard, and a page of the interface (the
// main menu) on a panel, pointed at with up to two pointers (the controllers'
// rays in a headset; the mouse on the desktop, so they can be tried
// without one). Says what each pointer is on, so a press on the controls
// isn't also the world's. What the menu holds is the app's (setMenu).

#include "RadialMenu.hpp"
#include "VrKeyboard.hpp"

#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace viewer
{

class VrControls
{
public:
    static constexpr int kPointers = 2; // left, right

    struct Pointer
    {
        bool active = false; // there's a ray (a tracked controller, the mouse)
        glm::vec3 origin{0.0f};
        glm::vec3 direction{1.0f, 0.0f, 0.0f};
        bool button = false;   // the trigger or the left mouse button: held
        bool menu = false;     // the menu button: held (held a moment: opens the menu)
        glm::vec2 stick{0.0f}; // a controller's stick (x right, y up)
        // Where the menu is if this pointer opens it (a controller: at the
        // hand, followed while it's open); nothing: in front of the eyes.
        std::optional<glm::vec3> menuAt;
    };
    struct Input
    {
        std::array<Pointer, kPointers> pointers;
        glm::vec3 eye{0.0f};
        glm::vec3 forward{1.0f, 0.0f, 0.0f}; // where the view looks
        // What the panels move with once placed (our avatar), so walking
        // takes them along.
        glm::vec3 anchor{0.0f};
        double now = 0.0; // seconds, for how long the menu button's held
    };
    // The menu button held this long opens the menu; let go sooner, it
    // closes an open one.
    static constexpr double kHoldToOpen = 0.5;
    // What a pointer is on after update().
    struct Pointed
    {
        bool onControls = false;
        std::optional<glm::vec3> at; // where its ray meets them
        // Its button went down this frame, not on the controls: the
        // world's press. And came up (the world's release).
        bool pressed = false;
        bool released = false;
    };

    void setMenu(MenuItem root) { menu_.setRoot(std::move(root)); }
    void setSubmit(VrKeyboard::Submit submit) { keyboard_.setSubmit(std::move(submit)); }
    // A press of the menu button (not a hold) while the radial menu's
    // closed.
    void setMainMenu(std::function<void()> f) { mainMenu_ = std::move(f); }
    // The keyboard, in front of the eyes (as last given to update()).
    void openKeyboard();

    // A page of the interface on a panel (the main menu), pointed at as the
    // keyboard is: its owner hears where the pointer on it is, its button,
    // and (a controller's stick) scrolling.
    struct Page
    {
        glm::vec2 size{0.0f}; // pixels
        float metersPerPixel = 0.0005f;
        SDL_GPUTexture* image = nullptr; // drawn on the panel
        // The pointer on it is here (nothing: none is now).
        std::function<void(std::optional<glm::vec2> pixel)> point;
        std::function<void(bool down)> button;
        std::function<void(glm::vec2 lines)> scroll; // positive: down, right
        std::function<std::string()> cursor;         // what a pointer on it shows (CSS names)
    };
    void setPage(Page page);
    // In front of the eyes (as last given to update()), moving with the
    // avatar; and put away.
    void openPage();
    // At `center`, facing `facing` (a point in front of it): it stays there
    // as long as the anchor given to update() does (the login lobby's
    // panels stand in the world).
    void openPageAt(glm::vec3 center, glm::vec3 facing, glm::vec3 anchor);
    void closePage();
    bool pageOpen() const { return pages_[0].open; }

    // More pages standing in the world (the lobby's boards: at `center`,
    // facing `facing`, not moving), pointed at as the page is; drawn with
    // it. Replaces those given before (none: none).
    struct StandingPage
    {
        Page page;
        glm::vec3 center{0.0f};
        glm::vec3 facing{1.0f, 0.0f, 0.0f};
    };
    void setStandingPages(std::vector<StandingPage> pages);
    // Scrolls the page a pointer is on (the mouse's wheel), in lines;
    // false if it's on none.
    bool scrollPointed(int pointer, glm::vec2 lines);
    // The cursor the page a pointer is on asks for (empty: the usual one,
    // or it's on none).
    std::string cursorPointed(int pointer) const;

    std::array<Pointed, kPointers> update(const Input& input);
    bool menuOpen() const { return menu_.isOpen(); }
    bool keyboardOpen() const { return keyboard_.isOpen(); }
    // The pointer that opened the menu (its stick points at it; on the
    // desktop, it wants the cursor).
    std::optional<int> menuPointer() const { return menuOpen() ? menuPointer_ : std::nullopt; }

    // The panels; and the pointers' rays (in a headset), each to what it's
    // on: the controls, else `worldHits`' point, else a way out.
    void addTo(render::Scene& scene, bool rays,
               const std::array<std::optional<glm::vec3>, kPointers>& worldHits) const;

    RadialMenu& menu() { return menu_; }
    VrKeyboard& keyboard() { return keyboard_; }

private:
    // A panel placed relative to the anchor when it opened.
    glm::mat4 anchored(const glm::mat4& frame, const glm::vec3& openedAt) const;

    RadialMenu menu_;
    VrKeyboard keyboard_;
    Input last_;
    std::array<Pointed, kPointers> pointed_{};
    std::array<bool, kPointers> button_{};
    std::array<bool, kPointers> menuButton_{};
    // When each pointer's menu button went down, while it's held and
    // hasn't opened the menu yet.
    std::array<std::optional<double>, kPointers> menuDownAt_{};
    // The slice the menu's hand's stick was last pushed to: chosen when the
    // stick's let go.
    std::optional<int> stickChoice_;
    std::function<void()> mainMenu_;
    struct Slot
    {
        Page page;
        bool open = false;
        glm::mat4 frame{1.0f};
        glm::vec3 anchor{0.0f};     // placed relative to it
        bool standing = false;      // not anchored: where it was put
        bool hovered = false;       // its owner was told a pointer's on it
        std::optional<int> pressed; // the pointer whose button is down on it
    };
    glm::mat4 slotFrame(const Slot& slot) const;
    // Tells a page's owner no pointer's on it or pressing it.
    static void leave(Slot& slot);
    // The page (openPage, openPageAt), then the standing ones.
    std::vector<Slot> pages_ = std::vector<Slot>(1);
    // The page each pointer is on (an index into pages_), and where.
    struct OnPage
    {
        size_t slot = 0;
        glm::vec2 pixel{0.0f};
    };
    std::array<std::optional<OnPage>, kPointers> onPage_{};
    double lastNow_ = 0.0;
    std::array<bool, kPointers> pressedOnControls_{}; // its press went to the controls
    std::optional<int> menuPointer_;
    glm::mat4 menuFrame_{1.0f};
    glm::vec3 menuAnchor_{0.0f};
    glm::mat4 keyboardFrame_{1.0f};
    glm::vec3 keyboardAnchor_{0.0f};
};

} // namespace viewer
