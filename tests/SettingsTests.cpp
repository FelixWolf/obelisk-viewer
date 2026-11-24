#include "Settings.hpp"
#include "TestUtil.hpp"
#include "ViewerSettings.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

using namespace viewer;

namespace
{

constexpr SettingKey<bool> kFlag{"test.flag"};
constexpr SettingKey<int32_t> kCount{"test.count"};
constexpr SettingKey<double> kScale{"test.scale"};
constexpr SettingKey<std::string> kName{"test.name"};
constexpr SettingKey<Uuid> kId{"test.id"};
constexpr SettingKey<std::string> kMode{"test.mode"};

std::vector<SettingDefinition> definitions()
{
    return {
        defineSetting(kFlag, true, {.page = "A", .label = "Flag"}),
        defineSetting(kCount, int32_t{10}, {.page = "A", .minimum = 0, .maximum = 100, .step = 5}),
        defineSetting(kScale, 1.5, {.page = "B", .minimum = 0.5, .maximum = 4.0}),
        defineSetting(kName, std::string("hello"),
                      {.page = "B",
                       .maxLength = 8,
                       .check = [](std::string_view s) -> std::string
                       { return s.find(' ') == std::string_view::npos ? "" : "No spaces."; }}),
        defineSetting(kId, Uuid(), {.page = "B"}),
        defineSetting(kMode, std::string("fast"),
                      {.page = "B", .choices = {{"fast", "Fast"}, {"slow", "Slow"}}}),
    };
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream(path, std::ios::binary) << text;
}

} // namespace

TEST_CASE("Settings: defaults, typed access and types")
{
    test::TempDirectory dir("settings_defaults");
    Settings s(definitions(), dir.path() / "settings.llsd");
    CHECK(s.get(kFlag));
    CHECK(s.get(kCount) == 10);
    CHECK(s.get(kScale) == 1.5);
    CHECK(s.get(kName) == "hello");
    CHECK(s.get(kId).isNull());
    CHECK(s.definition("test.mode")->type == SettingType::Choice);
    CHECK(s.definition("test.id")->type == SettingType::Uuid);
    CHECK(s.isDefault("test.count"));
    // A key of the wrong type doesn't set it.
    CHECK_FALSE(s.set(SettingKey<bool>{"test.count"}, true));
    CHECK_FALSE(s.set(SettingKey<bool>{"no.such"}, true));
}

TEST_CASE("Settings: set values are made ones the setting takes")
{
    test::TempDirectory dir("settings_set");
    Settings s(definitions(), dir.path() / "settings.llsd");
    CHECK(s.set(kCount, 500));
    CHECK(s.get(kCount) == 100); // clamped
    CHECK(s.set(kScale, 0.1));
    CHECK(s.get(kScale) == 0.5);
    CHECK_FALSE(s.set(kMode, std::string("medium")));
    CHECK(s.get(kMode) == "fast");
    CHECK(s.set(kMode, std::string("slow")));
    CHECK_FALSE(s.set(kName, std::string("has space")));
    CHECK_FALSE(s.set(kName, std::string("far too long")));

    const auto changed = s.takeChanged();
    CHECK(changed == std::vector<std::string>{"test.count", "test.scale", "test.mode"});
    CHECK(s.takeChanged().empty());
    s.reset("test.count");
    CHECK(s.get(kCount) == 10);
    CHECK(s.takeChanged() == std::vector<std::string>{"test.count"});
}

TEST_CASE("Settings: typed text is checked")
{
    test::TempDirectory dir("settings_text");
    Settings s(definitions(), dir.path() / "settings.llsd");
    CHECK(s.setText("test.count", " 42 ").empty());
    CHECK(s.get(kCount) == 42);
    CHECK(s.text("test.count") == "42");
    CHECK(s.setText("test.count", "4x") == "Must be a whole number, from 0 to 100.");
    CHECK(s.setText("test.count", "101") == "Must be a whole number, from 0 to 100.");
    CHECK(s.setText("test.count", "") == "Must be a whole number, from 0 to 100.");
    CHECK(s.setText("test.count", "99999999999").size() > 0);
    CHECK(s.get(kCount) == 42); // kept

    CHECK(s.setText("test.scale", "2.25").empty());
    CHECK(s.get(kScale) == 2.25);
    CHECK(s.setText("test.scale", "nan").size() > 0);
    CHECK(s.setText("test.scale", "9") == "Must be a number, from 0.5 to 4.0.");

    CHECK(s.setText("test.name", "a b") == "No spaces.");
    CHECK(s.setText("test.name", "123456789") == "Must be at most 8 characters.");
    CHECK(s.setText("test.name", "fine").empty());

    CHECK(s.setText("test.id", "not an id").starts_with("Must be an id"));
    CHECK(s.setText("test.id", "d7f4aeca-88f1-42a1-b385-b9db18abb255").empty());
    CHECK(s.text("test.id") == "d7f4aeca-88f1-42a1-b385-b9db18abb255");
    CHECK(s.setText("test.id", "").empty());
    CHECK(s.get(kId).isNull());
    CHECK(s.text("test.id").empty());

    CHECK(s.setText("test.mode", "Slow").empty()); // its label too
    CHECK(s.get(kMode) == "slow");
}

TEST_CASE("Settings: saved pretty, only what's changed, and read back")
{
    test::TempDirectory dir("settings_save");
    const auto file = dir.path() / "sub" / "settings.llsd";
    {
        Settings s(definitions(), file);
        s.set(kCount, 35);
        s.set(kId, Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255").value());
        s.set(kFlag, true); // the default: not written
        // Not yet: a moment after the change.
        s.saveIfChanged(10.0);
        CHECK_FALSE(std::filesystem::exists(file));
        s.saveIfChanged(10.5);
        CHECK_FALSE(std::filesystem::exists(file));
        s.saveIfChanged(11.0);
        REQUIRE(std::filesystem::exists(file));
    }
    CHECK(readFile(file) == "<?llsd/notation?>\n"
                            "{\n"
                            "    'test.count': i35,\n"
                            "    'test.id': ud7f4aeca-88f1-42a1-b385-b9db18abb255\n"
                            "}\n");
    Settings again(definitions(), file);
    CHECK(again.get(kCount) == 35);
    CHECK(again.get(kId).toString() == "d7f4aeca-88f1-42a1-b385-b9db18abb255");
    CHECK(again.takeChanged().empty()); // reading isn't changing

    // Back to its default: gone from the file (written when destroyed).
    {
        Settings s(definitions(), file);
        s.reset("test.count");
    }
    CHECK(readFile(file).find("test.count") == std::string::npos);
}

TEST_CASE("Settings: a file's odd values and unknown names")
{
    test::TempDirectory dir("settings_load");
    const auto file = dir.path() / "settings.llsd";
    writeFile(file, "{'test.count':i900,'test.flag':'yes','test.mode':'warp',"
                    "'test.scale':i3,'from.newer':[i1,i2]}");
    {
        Settings s(definitions(), file);
        CHECK(s.get(kCount) == 100);   // clamped
        CHECK(s.get(kFlag));           // not a boolean: the default
        CHECK(s.get(kMode) == "fast"); // not a choice: the default
        CHECK(s.get(kScale) == 3.0);   // an integer for a real
        s.set(kName, std::string("bye"));
    }
    // The unknown name is kept as it was.
    const auto back = llsd::parse(readFile(file));
    REQUIRE(back.has_value());
    CHECK((*back)["from.newer"].size() == 2);
    CHECK((*back)["test.name"].asString() == "bye");

    // Not LLSD at all: the defaults.
    writeFile(file, "{{{ nonsense");
    Settings broken(definitions(), file);
    CHECK(broken.get(kCount) == 10);
}

TEST_CASE("Settings: the viewer's are well defined")
{
    test::TempDirectory dir("settings_viewer");
    Settings s(viewerSettings(), dir.path() / "settings.llsd");
    std::vector<std::string> names;
    for(const SettingDefinition& d : s.definitions())
    {
        INFO(d.name);
        CHECK(!d.page.empty());
        CHECK(!d.label.empty());
        CHECK(s.isDefault(d.name));
        CHECK(std::ranges::find(names, d.name) == names.end());
        names.push_back(d.name);
        // Its default is a value it takes.
        CHECK(s.setValue(d.name, d.fallback));
        CHECK(s.takeChanged().empty());
    }
    CHECK(s.get(setting::kTextureMemory) == 2048);
    CHECK(s.get(setting::kStart) == "last");
}

TEST_CASE("Settings: overridden for a run, not saved")
{
    test::TempDirectory dir("settings_override");
    const auto file = dir.path() / "settings.llsd";
    writeFile(file, "{'test.count':i35}");
    {
        Settings s(definitions(), file);
        CHECK(s.override("test.count", "70").empty());
        CHECK(s.override("test.scale", "3").empty());
        CHECK(s.get(kCount) == 70);
        CHECK(s.isOverridden("test.count"));
        CHECK(s.takeChanged().empty()); // not a change: nothing's running yet
        CHECK(s.override("test.count", "abc") == "Must be a whole number, from 0 to 100.");
        CHECK(s.override("no.such", "1") == "there's no setting no.such");
        // Something else changed and saved: the overrides aren't.
        s.set(kFlag, false);
        CHECK(s.save());
        CHECK(readFile(file).find("'test.count': i35") != std::string::npos);
        CHECK(readFile(file).find("test.scale") == std::string::npos);
        // Changed again afterwards: that's kept.
        s.set(kCount, 80);
        CHECK_FALSE(s.isOverridden("test.count"));
        CHECK(s.save());
        CHECK(readFile(file).find("'test.count': i80") != std::string::npos);
    }
}

TEST_CASE("Settings: the viewer's hidden settings")
{
    test::TempDirectory dir("settings_hidden");
    Settings s(viewerSettings(), dir.path() / "settings.llsd");
    const SettingDefinition* cache = s.definition(setting::kCacheDirectory.name);
    REQUIRE(cache);
    CHECK(cache->hidden);
    CHECK(s.override("paths.cache", "/tmp/somewhere").empty());
    CHECK(s.get(setting::kCacheDirectory) == "/tmp/somewhere");
    CHECK(s.get(setting::kDayTime) < 0.0); // the region's
    CHECK(s.override("world.day_time", "0.5").empty());
    CHECK(s.override("world.day_time", "2").size() > 0);
    CHECK(s.override("network.viewerconfig", "ftp://x") ==
          "Must be an http(s):// address, or off.");
}
