#include "ScriptDialogs.hpp"

#include "viewer/protocol/Messages.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace viewer;
namespace msg = viewer::protocol::msg;

namespace
{

Uuid id(const char* text)
{
    return *Uuid::parse(text);
}

const Uuid kObject = id("11111111-1111-1111-1111-111111111111");
const Uuid kOther = id("22222222-2222-2222-2222-222222222222");
const Uuid kItem = id("33333333-3333-3333-3333-333333333333");
const Uuid kOwner = id("44444444-4444-4444-4444-444444444444");

msg::ScriptDialog dialog(const Uuid& object, std::vector<std::string> buttons,
                         std::string first = "Ann", std::string last = "Resident")
{
    msg::ScriptDialog m;
    m.Data.ObjectID = object;
    m.Data.FirstName = protocol::fromString(first);
    m.Data.LastName = protocol::fromString(last);
    m.Data.ObjectName = protocol::fromString("Vendor");
    m.Data.Message = protocol::fromString("Pick one");
    m.Data.ChatChannel = -42;
    for(const std::string& b : buttons)
    {
        m.Buttons.push_back({protocol::fromString(b)});
    }
    m.OwnerData.push_back({kOwner});
    return m;
}

msg::ScriptQuestion question(const Uuid& task, int32_t questions, std::string owner = "Bo Smith")
{
    msg::ScriptQuestion m;
    m.Data.TaskID = task;
    m.Data.ItemID = kItem;
    m.Data.ObjectName = protocol::fromString("Dance ball");
    m.Data.ObjectOwner = protocol::fromString(owner);
    m.Data.Questions = questions;
    return m;
}

} // namespace

TEST_CASE("ScriptDialogs: llDialog's buttons in rows of three, the first along the bottom")
{
    CHECK(dialogRows(0).empty());
    CHECK(dialogRows(2) == std::vector<std::vector<size_t>>{{0, 1}});
    CHECK(dialogRows(7) == std::vector<std::vector<size_t>>{{6}, {3, 4, 5}, {0, 1, 2}});
    CHECK(dialogRows(12).size() == 4);
    CHECK(dialogRows(12).front() == std::vector<size_t>{9, 10, 11});
}

TEST_CASE("ScriptDialogs: a dialog, its reply, and a newer one replacing it")
{
    ScriptDialogs d;
    d.dialog(7, dialog(kObject, {"Yes", "No"}));
    REQUIRE(d.front());
    CHECK(d.front()->kind == ScriptNotice::Kind::Dialog);
    CHECK(d.front()->owner == "Ann"); // " Resident" left off
    CHECK(d.front()->ownerId == kOwner);
    CHECK(d.front()->objectName == "Vendor");
    CHECK(d.front()->message == "Pick one");

    // The same object again: the newer replaces it; another's waits behind.
    d.dialog(7, dialog(kOther, {"A"}));
    d.dialog(7, dialog(kObject, {"Back", "Next"}));
    CHECK(d.waiting() == 2);
    CHECK(d.front()->object == kOther);

    d.dismiss(); // Ignore: no reply
    REQUIRE(d.front());
    CHECK(d.front()->buttons == std::vector<std::string>{"Back", "Next"});
    CHECK_FALSE(d.choose(2)); // no such button
    const auto r = d.choose(1);
    REQUIRE(r);
    CHECK(r->region == 7);
    CHECK(r->object == kObject);
    CHECK(r->channel == -42);
    CHECK(r->index == 1);
    CHECK(r->label == "Next");
    CHECK_FALSE(d.front());
}

TEST_CASE("ScriptDialogs: a group's object, and at most 12 buttons")
{
    ScriptDialogs d;
    std::vector<std::string> many;
    for(int i = 0; i < 14; i++)
    {
        many.push_back(std::to_string(i));
    }
    d.dialog(1, dialog(kObject, many, "", "The Group"));
    REQUIRE(d.front());
    CHECK(d.front()->groupOwned);
    CHECK(d.front()->owner == "The Group");
    CHECK(d.front()->buttons.size() == ScriptDialogs::kMaxButtons);
}

TEST_CASE("ScriptDialogs: a text box replies with its text, cut to whole characters")
{
    ScriptDialogs d;
    d.dialog(3, dialog(kObject, {std::string(ScriptDialogs::kTextBoxButton)}));
    REQUIRE(d.front());
    CHECK(d.front()->kind == ScriptNotice::Kind::TextBox);
    CHECK_FALSE(d.choose(0)); // not a dialog's button

    // 253 bytes, then a two-byte character: it doesn't fit whole.
    const std::string text = std::string(253, 'a') + "\xc3\xa9";
    const auto r = d.reply(text);
    REQUIRE(r);
    CHECK(r->index == 0);
    CHECK(r->label == std::string(253, 'a'));
}

TEST_CASE("ScriptDialogs: permissions asked, allowed or denied; unknown ones never granted")
{
    ScriptDialogs d;
    const int32_t animate = 1 << 4;
    const int32_t debit = 1 << 1;
    const int32_t unknown = 1 << 30;
    d.question(5, question(kObject, animate | debit | unknown), "Me", 0.0);
    REQUIRE(d.front());
    CHECK(d.front()->kind == ScriptNotice::Kind::Permissions);
    CHECK(d.front()->questions == (animate | debit));
    CHECK(d.front()->permissions.size() == 2);
    CHECK(d.front()->caution); // money
    const auto allowed = d.answer(true);
    REQUIRE(allowed);
    CHECK(allowed->region == 5);
    CHECK(allowed->task == kObject);
    CHECK(allowed->item == kItem);
    CHECK(allowed->questions == (animate | debit));

    d.question(5, question(kObject, animate), "Me", 1.0);
    const auto denied = d.answer(false);
    REQUIRE(denied);
    CHECK(denied->questions == 0);

    // Ignored is denied.
    d.question(5, question(kObject, animate), "Me", 2.0);
    const auto ignored = d.dismiss();
    REQUIRE(ignored);
    CHECK(ignored->questions == 0);

    // Only unknown ones: nothing to ask.
    d.question(5, question(kObject, unknown), "Me", 3.0);
    CHECK_FALSE(d.front());
}

TEST_CASE("ScriptDialogs: an owner's permission requests are throttled, ours by object")
{
    ScriptDialogs d;
    for(int i = 0; i < 7; i++)
    {
        d.question(1, question(kObject, 1 << 4), "Me", i * 0.5);
    }
    CHECK(d.waiting() == 5);                              // the rest dropped
    d.question(1, question(kObject, 1 << 4), "Me", 20.0); // later: asked again
    CHECK(d.waiting() == 6);

    // Our own objects: each counted on its own.
    ScriptDialogs ours;
    for(int i = 0; i < 6; i++)
    {
        ours.question(1, question(kObject, 1 << 4, "Me Resident"), "Me", 0.0);
    }
    CHECK(ours.waiting() == 5); // that object's sixth dropped
    ours.question(1, question(kOther, 1 << 4, "Me Resident"), "Me", 0.0);
    CHECK(ours.waiting() == 6); // another of ours isn't
}

TEST_CASE("ScriptDialogs: web pages offered, opened or not; only the web's")
{
    ScriptDialogs d;
    msg::LoadURL m;
    m.Data.ObjectID = kObject;
    m.Data.OwnerID = kOwner;
    m.Data.ObjectName = protocol::fromString("Sign");
    m.Data.Message = protocol::fromString("Our site");
    m.Data.URL = protocol::fromString("https://example.com/");
    d.loadUrl(9, m);
    REQUIRE(d.front());
    CHECK(d.front()->kind == ScriptNotice::Kind::LoadUrl);
    CHECK(d.front()->ownerId == kOwner);
    CHECK(d.open() == "https://example.com/");
    CHECK_FALSE(d.front());

    m.Data.URL = protocol::fromString("file:///etc/passwd");
    d.loadUrl(9, m);
    CHECK_FALSE(d.front());
}

TEST_CASE("ScriptDialogs: answers only fit what's shown; logging out clears them")
{
    ScriptDialogs d;
    d.dialog(1, dialog(kObject, {"OK"}));
    CHECK_FALSE(d.answer(true));
    CHECK_FALSE(d.open());
    CHECK_FALSE(d.reply("x"));
    const uint64_t before = d.version();
    d.clear();
    CHECK_FALSE(d.front());
    CHECK(d.version() != before);
}
