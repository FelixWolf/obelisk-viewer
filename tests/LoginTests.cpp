#include "TestUtil.hpp"

#include "viewer/core/Md5.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/session/Credentials.hpp"
#include "viewer/session/Login.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

using namespace viewer::session;
namespace llsd = viewer::llsd;

namespace
{

// Shaped like a real login.cgi success reply (trimmed).
constexpr std::string_view kSuccessReply = R"(<?xml version="1.0" ?>
<llsd><map>
  <key>login</key><string>true</string>
  <key>first_name</key><string>"Test"</string>
  <key>last_name</key><string>Resident</string>
  <key>agent_id</key><uuid>d7f4aeca-88f1-42a1-b385-b9db18abb255</uuid>
  <key>session_id</key><uuid>11111111-2222-3333-4444-555555555555</uuid>
  <key>secure_session_id</key><uuid>66666666-7777-8888-9999-aaaaaaaaaaaa</uuid>
  <key>circuit_code</key><integer>3000000000</integer>
  <key>sim_ip</key><string>192.0.2.10</string>
  <key>sim_port</key><integer>13005</integer>
  <key>region_x</key><integer>256000</integer>
  <key>region_y</key><integer>257024</integer>
  <key>seed_capability</key><string>https://sim.example/cap/abc</string>
  <key>message</key><string>Welcome!</string>
  <key>look_at</key><string>[r0.9,r0.1,r0]</string>
</map></llsd>)";

llsd::Value parseXml(std::string_view xml)
{
    auto value = llsd::parse(xml);
    REQUIRE(value.has_value());
    return std::move(*value);
}

} // namespace

TEST_CASE("Login password hash is $1$ + md5, truncated to 16 characters")
{
    CHECK(hashPassword("secret") == "$1$" + viewer::md5Hex("secret"));
    CHECK(hashPassword("0123456789abcdefTHIS_IS_IGNORED") == "$1$4032af8d61035123906e58e067140cc5");
    CHECK(hashPassword("$1$abcdef") == "$1$abcdef"); // already hashed
}

TEST_CASE("Login usernames")
{
    CHECK(parseAgentName("Test Resident").value().first == "Test");
    CHECK(parseAgentName("Test Resident").value().last == "Resident");
    CHECK(parseAgentName("first.last").value().last == "last");
    CHECK(parseAgentName("  solo  ").value().first == "solo");
    CHECK(parseAgentName("solo").value().last == "Resident");
    CHECK(parseAgentName("trailing.").value().last == "Resident");
    CHECK_FALSE(parseAgentName("").has_value());
    CHECK_FALSE(parseAgentName("   ").has_value());
    CHECK_FALSE(parseAgentName(".last").has_value());
    CHECK_FALSE(parseAgentName("a b c").has_value());
}

TEST_CASE("Login request contains the expected fields and no plain password")
{
    LoginParameters p;
    p.name = {"Test", "Resident"};
    p.passwordHash = hashPassword("hunter2");
    p.channel = "viewer";
    p.version = "0.1.0";
    p.machine = {.platform = "Lnx",
                 .platformVersion = "2.42.0",
                 .platformString = "Linux 6.0",
                 .mac = "0123456789abcdef0123456789abcdef",
                 .id0 = "613b85f2def676b5a7f9318183da00e6"};

    const llsd::Value request = buildLoginRequest(p);
    CHECK(request["first"].asString() == "Test");
    CHECK(request["last"].asString() == "Resident");
    CHECK(request["passwd"].asString() == p.passwordHash);
    CHECK(request["mac"].asString() == p.machine.mac);
    CHECK(request["platform_string"].asString() == "Linux 6.0");
    CHECK(request["address_size"].asInteger() == 64);
    CHECK(request["id0"].asString() == p.machine.id0);
    CHECK(request["start"].asString() == "last");
    CHECK(request["agent_flags"].asInteger() == 0);
    CHECK(request["agree_to_tos"].type() == llsd::Value::Type::Boolean);
    CHECK_FALSE(request["agree_to_tos"].asBoolean());
    CHECK(request["options"].size() == defaultLoginOptions().size());

    const std::string xml = llsd::toXml(request);
    CHECK(xml.find("hunter2") == std::string::npos);
}

TEST_CASE("queryMachineInfo uses the official viewer's formats")
{
    const MachineInfo info = queryMachineInfo();
    auto isMd5Hex = [](const std::string& s)
    { return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos; };
    CHECK_FALSE(info.platform.empty());
    CHECK(isMd5Hex(info.mac)); // never the raw MAC address
    CHECK(isMd5Hex(info.id0));
    // "major.minor.build"
    CHECK(std::ranges::count(info.platformVersion, '.') == 2);
    CHECK_FALSE(info.platformString.empty());
}

TEST_CASE("Login reply: success")
{
    const auto result = parseLoginReply(parseXml(kSuccessReply));
    REQUIRE(result.has_value());
    CHECK(result->name.first == "Test"); // the quotes are stripped
    CHECK(result->name.last == "Resident");
    CHECK(result->agentId.toString() == "d7f4aeca-88f1-42a1-b385-b9db18abb255");
    CHECK(result->circuitCode == 3000000000u);
    CHECK(result->simIp == "192.0.2.10");
    CHECK(result->simPort == 13005);
    CHECK(result->regionX / 256 == 1000);
    CHECK(result->regionY / 256 == 1004);
    CHECK(result->seedCapability == "https://sim.example/cap/abc");
    CHECK(result->message == "Welcome!");
    CHECK(result->reply["look_at"].asString() == "[r0.9,r0.1,r0]");
}

TEST_CASE("Login reply: failures")
{
    const auto key = parseLoginReply(parseXml(R"(<llsd><map>
        <key>login</key><string>false</string>
        <key>reason</key><string>key</string>
        <key>message</key><string>Sorry! We couldn't log you in.</string>
        </map></llsd>)"));
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error().reason == "key");
    CHECK(key.error().message == "Sorry! We couldn't log you in.");

    const auto mfa = parseLoginReply(parseXml(R"(<llsd><map>
        <key>login</key><string>false</string>
        <key>reason</key><string>mfa_challenge</string>
        <key>message</key><string>Enter your code</string>
        </map></llsd>)"));
    REQUIRE_FALSE(mfa.has_value());
    CHECK(mfa.error().reason == "mfa_challenge");

    const auto incomplete = parseLoginReply(
        parseXml(R"(<llsd><map><key>login</key><string>true</string></map></llsd>)"));
    REQUIRE_FALSE(incomplete.has_value());
    CHECK(incomplete.error().reason == "incomplete");
    CHECK(incomplete.error().message.find("seed_capability") != std::string::npos);

    const auto indeterminate = parseLoginReply(
        parseXml(R"(<llsd><map><key>login</key><string>indeterminate</string></map></llsd>)"));
    REQUIRE_FALSE(indeterminate.has_value());
    CHECK(indeterminate.error().reason == "indeterminate");

    CHECK(parseLoginReply(llsd::Value(5)).error().reason == "llsd");
}

TEST_CASE("Credentials files")
{
    viewer::test::TempDirectory dir("credentials");
    auto write = [&](const std::string& name, const std::string& text)
    {
        const auto path = dir.path() / name;
        std::ofstream(path) << text;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write);
        return path;
    };

    const auto plain = readCredentialsFile(write("plain.txt", "Test Resident\nsecret\n"));
    REQUIRE(plain.has_value());
    CHECK(plain->name.first == "Test");
    CHECK(plain->name.last == "Resident");
    CHECK(plain->passwordHash == hashPassword("secret"));
    CHECK(plain->loginUri == kAditiLoginUri); // test grid unless told otherwise

    const std::string hash = hashPassword("secret");
    const auto hashed =
        readCredentialsFile(write("hashed.txt", "tester\r\n  " + hash + "  \r\nhttps://x/login\n"));
    REQUIRE(hashed.has_value());
    CHECK(hashed->name.last == "Resident");
    CHECK(hashed->passwordHash == hash);
    CHECK(hashed->loginUri == "https://x/login");

    CHECK_FALSE(readCredentialsFile(write("short.txt", "only a name\n")).has_value());
    CHECK_FALSE(readCredentialsFile(write("blank.txt", "name\n\n")).has_value());
    CHECK_FALSE(readCredentialsFile(dir.path() / "missing.txt").has_value());
    CHECK_FALSE(readCredentialsFile(dir.path()).has_value()); // a directory
}
