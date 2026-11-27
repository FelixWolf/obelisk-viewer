#include "PanelGeometry.hpp"
#include "RadialMenu.hpp"
#include "VrControls.hpp"
#include "VrKeyboard.hpp"
#include "VrTurning.hpp"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

using namespace viewer;

namespace
{

// A menu facing a viewer at the origin from 1 m ahead (+X).
glm::mat4 ahead()
{
    return panelFacing(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f));
}

} // namespace

TEST_CASE("RadialMenu: actions, submenus and back")
{
    std::vector<std::string> ran;
    bool flying = false;
    RadialMenu menu;
    menu.setRoot(MenuItem::submenu(
        "", {MenuItem::run("Fly", [&] { flying = !flying; })
                 .named([&] { return flying ? std::string("Land") : std::string("Fly"); })
                 .staying(),
             MenuItem::run("Stand up", [&] { ran.push_back("stand"); }).when([] { return false; }),
             MenuItem::submenu("More", {MenuItem::run("Deep", [&] { ran.push_back("deep"); })})}));
    menu.open(ahead());
    REQUIRE(menu.slots().size() == 3); // no Back at the top
    CHECK(menu.title().empty());

    // A toggle that keeps the menu open, its label following.
    menu.hover(0);
    CHECK(menu.choose());
    CHECK(flying);
    CHECK(menu.isOpen());
    CHECK(menu.slots()[0].label == "Land");

    // Disabled: not chosen.
    menu.hover(1);
    CHECK_FALSE(menu.slots()[1].enabled);
    CHECK_FALSE(menu.choose());
    CHECK(ran.empty());

    // Into the submenu, back out, in again and run: the menu closes.
    menu.hover(2);
    CHECK(menu.slots()[2].label == "More >");
    CHECK(menu.choose());
    CHECK(menu.depth() == 2);
    CHECK(menu.title() == "More");
    REQUIRE(menu.slots().size() == 2);
    CHECK(menu.slots()[1].kind == RadialMenu::Slot::Kind::Back);
    menu.hover(1);
    CHECK(menu.choose());
    CHECK(menu.depth() == 1);
    menu.hover(2);
    menu.choose();
    menu.hover(0);
    CHECK(menu.choose());
    CHECK(ran == std::vector<std::string>{"deep"});
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("RadialMenu: a level with more items than slices pages")
{
    std::vector<MenuItem> items;
    for(int i = 0; i < 17; i++)
    {
        items.push_back(MenuItem::run(std::format("Item {}", i), [] {}));
    }
    RadialMenu menu;
    menu.setRoot(MenuItem::submenu("", std::move(items)));
    menu.open(ahead());
    // 7 to a page and a page turn: 3 pages.
    CHECK(menu.pages() == 3);
    auto slots = menu.slots();
    REQUIRE(slots.size() == RadialMenu::kSlices);
    CHECK(slots.back().kind == RadialMenu::Slot::Kind::NextPage);
    CHECK(slots.back().label == "More 1/3");
    menu.hover(RadialMenu::kSlices - 1);
    menu.choose();
    CHECK(menu.page() == 1);
    CHECK(menu.slots()[0].label == "Item 7");
    menu.hover(RadialMenu::kSlices - 1);
    menu.choose();
    slots = menu.slots();
    CHECK(slots.size() == 4); // 14, 15, 16 and the turn
    CHECK(slots[0].label == "Item 14");
    menu.hover(3);
    menu.choose();
    CHECK(menu.page() == 0); // round to the first

    // Exactly kSlices: one page, no turn.
    std::vector<MenuItem> eight;
    for(int i = 0; i < RadialMenu::kSlices; i++)
    {
        eight.push_back(MenuItem::run("x", [] {}));
    }
    menu.setRoot(MenuItem::submenu("", std::move(eight)));
    menu.open(ahead());
    CHECK(menu.pages() == 1);
    CHECK(menu.slots().size() == RadialMenu::kSlices);

    // In a submenu, Back takes a slice: 7 items fit, 8 page (6 a page).
    std::vector<MenuItem> sub;
    for(int i = 0; i < RadialMenu::kSlices; i++)
    {
        sub.push_back(MenuItem::run("y", [] {}));
    }
    std::vector<MenuItem> top;
    top.push_back(MenuItem::submenu("Sub", std::move(sub)));
    menu.setRoot(MenuItem::submenu("", std::move(top)));
    menu.open(ahead());
    menu.hover(0);
    menu.choose();
    CHECK(menu.pages() == 2);
    slots = menu.slots();
    REQUIRE(slots.size() == RadialMenu::kSlices);
    CHECK(slots[6].kind == RadialMenu::Slot::Kind::NextPage);
    CHECK(slots[7].kind == RadialMenu::Slot::Kind::Back);
}

TEST_CASE("RadialMenu: pointed at by stick and by ray")
{
    RadialMenu menu;
    std::vector<MenuItem> four;
    for(int i = 0; i < 4; i++)
    {
        four.push_back(MenuItem::run("x", [] {}));
    }
    menu.setRoot(MenuItem::submenu("", std::move(four)));
    menu.open(ahead());
    // Clockwise from the top: up, right, down, left.
    menu.pointToward({0.0f, 1.0f});
    CHECK(menu.hovered() == 0);
    menu.pointToward({1.0f, 0.0f});
    CHECK(menu.hovered() == 1);
    menu.pointToward({0.0f, -1.0f});
    CHECK(menu.hovered() == 2);
    menu.pointToward({-0.9f, 0.1f});
    CHECK(menu.hovered() == 3);
    CHECK(menu.cursor());
    menu.pointToward({0.1f, 0.1f}); // let be: the middle, nothing
    CHECK_FALSE(menu.hovered());
    CHECK(glm::length(*menu.cursor() - menu.panel().size * 0.5f) < 46.0f);
    menu.clearPoint();
    CHECK_FALSE(menu.cursor());

    // A ray at the middle; one at the right slice (the viewer's right is -Y
    // looking along +X); one that misses.
    CHECK(menu.pointRay(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK_FALSE(menu.hovered());
    CHECK(menu.pointRay(glm::vec3(0.0f), glm::normalize(glm::vec3(1.0f, -0.11f, 0.0f))));
    CHECK(menu.hovered() == 1);
    CHECK_FALSE(menu.pointRay(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f)));
}

TEST_CASE("VrKeyboard: typing, shift, backspace, send")
{
    VrKeyboard keyboard;
    std::string sent;
    keyboard.setSubmit([&](const std::string& text) { sent = text; });
    keyboard.open(ahead());
    auto find = [&](VrKeyboard::Key::Kind kind, std::string_view lower = {})
    {
        const auto& keys = keyboard.keys();
        for(size_t i = 0; i < keys.size(); i++)
        {
            if(keys[i].kind == kind && (lower.empty() || keys[i].lower == lower))
            {
                return i;
            }
        }
        FAIL("no such key");
        return size_t{0};
    };
    using Kind = VrKeyboard::Key::Kind;
    keyboard.pressKey(find(Kind::Shift));
    keyboard.pressKey(find(Kind::Character, "h"));
    keyboard.pressKey(find(Kind::Character, "i"));
    keyboard.pressKey(find(Kind::Character, "1")); // shifted once only
    CHECK(keyboard.text() == "Hi1");
    keyboard.pressKey(find(Kind::Backspace));
    keyboard.pressKey(find(Kind::Space));
    keyboard.pressKey(find(Kind::Character, "x"));
    CHECK(keyboard.text() == "Hi x");
    keyboard.pressKey(find(Kind::Enter));
    CHECK(sent == "Hi x");
    CHECK(keyboard.text().empty());
    CHECK_FALSE(keyboard.isOpen());

    // Every key can be pointed at where it's drawn.
    keyboard.open(ahead());
    const render::Panel panel = keyboard.panel();
    CHECK(panel.size.x > 0.0f);
}

TEST_CASE("VrControls: the menu opened by a hold, closed by a press; presses kept from the world")
{
    int ran = 0;
    VrControls controls;
    controls.setMenu(
        MenuItem::submenu("", {MenuItem::run("A", [&] { ran++; }), MenuItem::run("B", [] {}),
                               MenuItem::run("C", [] {}), MenuItem::run("D", [] {})}));
    VrControls::Input input;
    input.eye = glm::vec3(0.0f);
    input.forward = glm::vec3(1.0f, 0.0f, 0.0f);
    VrControls::Pointer& mouse = input.pointers[1];
    mouse.active = true;
    mouse.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    auto later = [&](double seconds) { input.now += seconds; };

    // A press on nothing is the world's.
    mouse.button = true;
    auto pointed = controls.update(input);
    CHECK(pointed[1].pressed);
    mouse.button = false;
    pointed = controls.update(input);
    CHECK(pointed[1].released);

    int mainMenu = 0;
    controls.setMainMenu([&] { mainMenu++; });

    // A quick press of the menu button: the main menu, not the radial one.
    mouse.menu = true;
    controls.update(input);
    later(0.2);
    mouse.menu = false;
    controls.update(input);
    CHECK_FALSE(controls.menuOpen());
    CHECK(mainMenu == 1);

    // Held a moment: it opens, and stays open when let go.
    mouse.menu = true;
    controls.update(input);
    later(0.3);
    controls.update(input);
    CHECK_FALSE(controls.menuOpen());
    later(0.3);
    controls.update(input);
    REQUIRE(controls.menuOpen());
    mouse.menu = false;
    later(0.1);
    controls.update(input);
    CHECK(controls.menuOpen());

    // The ray on the top slice; a click on it is the menu's, not the world's.
    mouse.direction = glm::normalize(glm::vec3(0.5f, 0.0f, 0.06f));
    pointed = controls.update(input);
    CHECK(pointed[1].onControls);
    CHECK(controls.menu().hovered() == 0);
    mouse.button = true;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].pressed);
    CHECK(ran == 1);
    CHECK_FALSE(controls.menuOpen()); // an action closes it
    mouse.button = false;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].released);

    // Opened again; a quick press closes it.
    mouse.menu = true;
    controls.update(input);
    later(0.6);
    controls.update(input);
    mouse.menu = false;
    controls.update(input);
    REQUIRE(controls.menuOpen());
    mouse.menu = true;
    later(0.1);
    controls.update(input);
    CHECK(controls.menuOpen());
    mouse.menu = false;
    later(0.1);
    controls.update(input);
    CHECK_FALSE(controls.menuOpen());

    CHECK(mainMenu == 1); // closing it wasn't the main menu

    // The middle, clicked: nothing, still open, and not the world's.
    mouse.menu = true;
    controls.update(input);
    later(0.6);
    controls.update(input);
    mouse.menu = false;
    controls.update(input);
    REQUIRE(controls.menuOpen());
    mouse.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    pointed = controls.update(input);
    CHECK(pointed[1].onControls);
    CHECK_FALSE(controls.menu().hovered());
    mouse.button = true;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].pressed);
    CHECK(controls.menuOpen());
}

TEST_CASE("VrControls: the menu follows the hand that opened it; its stick, let go, chooses")
{
    int ran = 0;
    VrControls controls;
    controls.setMenu(
        MenuItem::submenu("", {MenuItem::run("A", [&] { ran++; }), MenuItem::run("B", [] {})}));
    VrControls::Input input;
    input.eye = glm::vec3(0.0f, 0.0f, 1.6f);
    VrControls::Pointer& left = input.pointers[0];
    left.active = true;
    left.menuAt = glm::vec3(0.4f, 0.2f, 1.3f);
    left.menu = true;
    controls.update(input);
    input.now += 0.6;
    controls.update(input);
    left.menu = false;
    controls.update(input);
    REQUIRE(controls.menuOpen());
    CHECK(controls.menuPointer() == 0);
    CHECK(glm::length(glm::vec3(controls.menu().panel().frame[3]) - *left.menuAt) < 1e-4f);

    left.menuAt = glm::vec3(0.5f, -0.1f, 1.2f);
    controls.update(input);
    CHECK(glm::length(glm::vec3(controls.menu().panel().frame[3]) - *left.menuAt) < 1e-4f);

    // The stick let be: its cursor in the middle, nothing pointed at; the
    // trigger is the world's.
    auto pointed = controls.update(input);
    CHECK_FALSE(controls.menu().hovered());
    CHECK(controls.menu().panel().over.size() == 1);
    left.button = true;
    pointed = controls.update(input);
    CHECK(pointed[0].pressed);
    CHECK(controls.menuOpen());
    left.button = false;
    controls.update(input);

    // The stick up to the top slice, then let go: chosen.
    left.stick = glm::vec2(0.0f, 1.0f);
    controls.update(input);
    CHECK(controls.menu().hovered() == 0);
    left.stick = glm::vec2(0.0f, 0.4f); // on its way back: still the one
    controls.update(input);
    CHECK(controls.menu().hovered() == 0);
    CHECK(ran == 0);
    left.stick = glm::vec2(0.0f);
    controls.update(input);
    CHECK(ran == 1);
    CHECK_FALSE(controls.menuOpen());
}

TEST_CASE("VrControls: the menu can be made anew as it opens (for what's pointed at)")
{
    int opened = 0;
    VrControls controls;
    controls.setMenu(MenuItem::submenu("", {MenuItem::run("Old", [] {})}));
    controls.setMenuOpening(
        [&]
        {
            opened++;
            controls.setMenu(MenuItem::submenu(
                "", {MenuItem::run("Sit here", [] {}), MenuItem::run("Fly", [] {})}));
        });
    VrControls::Input input;
    input.eye = glm::vec3(0.0f, 0.0f, 1.6f);
    VrControls::Pointer& left = input.pointers[0];
    left.active = true;
    left.menuAt = glm::vec3(0.4f, 0.2f, 1.3f);
    left.menu = true;
    controls.update(input);
    CHECK(opened == 0);
    input.now += 0.6;
    controls.update(input);
    REQUIRE(controls.menuOpen());
    CHECK(opened == 1);
    const auto slots = controls.menu().slots();
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].label == "Sit here");
    // Held on (moving it to the hand again): not made anew.
    input.now += 0.6;
    controls.update(input);
    CHECK(opened == 1);
}

TEST_CASE("VrControls: a page on a panel: pointed at, pressed, not the world's")
{
    VrControls controls;
    std::vector<std::optional<glm::vec2>> points;
    std::vector<bool> buttons;
    VrControls::Page page;
    page.size = glm::vec2(1000.0f, 500.0f);
    page.metersPerPixel = 0.001f;
    page.point = [&](std::optional<glm::vec2> pixel) { points.push_back(pixel); };
    page.button = [&](bool down) { buttons.push_back(down); };
    controls.setPage(std::move(page));

    VrControls::Input input;
    input.eye = glm::vec3(0.0f);
    input.forward = glm::vec3(1.0f, 0.0f, 0.0f);
    VrControls::Pointer& mouse = input.pointers[1];
    mouse.active = true;
    mouse.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    controls.update(input);
    controls.openPage();
    REQUIRE(controls.pageOpen());

    // Straight ahead (a little above its middle: it's placed below the
    // eyes): on it.
    auto pointed = controls.update(input);
    CHECK(pointed[1].onControls);
    REQUIRE_FALSE(points.empty());
    REQUIRE(points.back());
    CHECK(points.back()->x == doctest::Approx(500.0f).epsilon(0.01));
    CHECK(points.back()->y < 250.0f);

    // A press on it is the page's.
    mouse.button = true;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].pressed);
    REQUIRE(buttons.size() == 1);
    CHECK(buttons[0]);
    mouse.button = false;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].released);
    REQUIRE(buttons.size() == 2);
    CHECK_FALSE(buttons[1]);

    // Off it: told so once; presses are the world's again.
    mouse.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    controls.update(input);
    CHECK_FALSE(points.back());
    const size_t told = points.size();
    controls.update(input);
    CHECK(points.size() == told);
    mouse.button = true;
    pointed = controls.update(input);
    CHECK(pointed[1].pressed);
}

TEST_CASE("VrControls: standing pages: the nearest pointed at, pressed and scrolled")
{
    VrControls controls;
    struct Heard
    {
        std::vector<std::optional<glm::vec2>> points;
        std::vector<bool> buttons;
        std::vector<glm::vec2> scrolls;
    };
    Heard near, far;
    auto page = [](Heard& heard)
    {
        VrControls::Page p;
        p.size = glm::vec2(1000.0f, 1000.0f);
        p.metersPerPixel = 0.001f;
        p.point = [&heard](std::optional<glm::vec2> pixel) { heard.points.push_back(pixel); };
        p.button = [&heard](bool down) { heard.buttons.push_back(down); };
        p.scroll = [&heard](glm::vec2 lines) { heard.scrolls.push_back(lines); };
        return p;
    };
    // Both ahead (+X), facing back at us; one nearer.
    std::vector<VrControls::StandingPage> pages;
    pages.push_back({page(far), glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.0f)});
    pages.push_back({page(near), glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f)});
    controls.setStandingPages(std::move(pages));

    VrControls::Input input;
    VrControls::Pointer& mouse = input.pointers[1];
    mouse.active = true;
    mouse.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    auto pointed = controls.update(input);
    CHECK(pointed[1].onControls);
    REQUIRE_FALSE(near.points.empty());
    CHECK(near.points.back());
    CHECK(far.points.empty());

    // The wheel: the page it's on.
    CHECK(controls.scrollPointed(1, glm::vec2(0.0f, 2.0f)));
    REQUIRE(near.scrolls.size() == 1);
    CHECK(near.scrolls[0].y == 2.0f);
    CHECK_FALSE(controls.scrollPointed(0, glm::vec2(0.0f, 2.0f))); // the left: on nothing

    // A press is the page's, not the world's.
    mouse.button = true;
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].pressed);
    REQUIRE(near.buttons.size() == 1);
    CHECK(near.buttons[0]);
    mouse.button = false;
    controls.update(input);
    REQUIRE(near.buttons.size() == 2);

    // Gone: told the pointer's left it.
    controls.setStandingPages({});
    CHECK_FALSE(near.points.back());
    pointed = controls.update(input);
    CHECK_FALSE(pointed[1].onControls);
    CHECK_FALSE(controls.scrollPointed(1, glm::vec2(0.0f, 1.0f)));
}

TEST_CASE("VrTurning: a step a flick, or smoothly")
{
    VrTurning turning;
    turning.snapAngle = 45.0f;
    CHECK(turning.update(0.5f, 0.01f) == 0.0f);
    CHECK(turning.update(1.0f, 0.01f) == -45.0f); // right: clockwise
    CHECK(turning.update(1.0f, 0.01f) == 0.0f);   // held
    CHECK(turning.update(0.5f, 0.01f) == 0.0f);   // not let back far enough
    CHECK(turning.update(1.0f, 0.01f) == 0.0f);
    CHECK(turning.update(0.0f, 0.01f) == 0.0f);
    CHECK(turning.update(-1.0f, 0.01f) == 45.0f);

    turning.smooth = true;
    turning.speed = 100.0f;
    CHECK(turning.update(0.1f, 0.01f) == 0.0f); // the dead zone
    CHECK(turning.update(1.0f, 0.01f) == doctest::Approx(-1.0f));
    CHECK(turning.update(-0.6f, 0.01f) == doctest::Approx(0.5f));
    CHECK(turning.update(1.0f, 5.0f) == doctest::Approx(-10.0f)); // a hitch

    // Switched with the stick held: no step until it's let go.
    turning.smooth = false;
    CHECK(turning.update(1.0f, 0.01f) == 0.0f);
    turning.update(0.0f, 0.01f);
    CHECK(turning.update(1.0f, 0.01f) == -45.0f);
}
