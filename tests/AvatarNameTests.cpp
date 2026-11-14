#include "AvatarNames.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

using namespace viewer;

TEST_CASE("Avatar names: a GetDisplayNames agent, and usernames")
{
    const auto body = llsd::parse(std::string_view(
        "<llsd><map><key>agents</key><array><map>"
        "<key>id</key><uuid>6f1cbf7d-7945-47cb-8428-8cc14a60e94c</uuid>"
        "<key>username</key><string>automatedtests</string>"
        "<key>display_name</key><string>Automated Tests</string>"
        "<key>legacy_first_name</key><string>AutomatedTests</string>"
        "<key>legacy_last_name</key><string>Resident</string>"
        "<key>is_display_name_default</key><boolean>0</boolean>"
        "</map><map><key>username</key><string>no id</string></map></array></map></llsd>"));
    REQUIRE(body);
    const llsd::Value& agents = (*body)["agents"];
    REQUIRE(agents.size() == 2);
    const auto parsed = parseAvatarName(agents[0]);
    REQUIRE(parsed);
    CHECK(parsed->first == *Uuid::parse("6f1cbf7d-7945-47cb-8428-8cc14a60e94c"));
    CHECK(parsed->second.displayName == "Automated Tests");
    CHECK_FALSE(parsed->second.displayNameDefault);
    // "Resident" isn't shown.
    CHECK(parsed->second.userName() == "AutomatedTests");
    CHECK_FALSE(parseAvatarName(agents[1]));

    AvatarName old;
    old.legacyFirstName = "Kyler";
    old.legacyLastName = "Eastridge";
    CHECK(old.userName() == "Kyler Eastridge");
    // No display name: the username stands in.
    const auto bare = llsd::parse(
        std::string_view("<llsd><map><key>id</key><uuid>6f1cbf7d-7945-47cb-8428-8cc14a60e94c</uuid>"
                         "<key>username</key><string>someone</string></map></llsd>"));
    REQUIRE(bare);
    CHECK(parseAvatarName(*bare)->second.displayName == "someone");
}
