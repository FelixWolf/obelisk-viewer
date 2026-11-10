#pragma once

// The XML-RPC-replacement login: one LLSD+XML POST to the grid's login URI.
// Pure data in, pure data out; Session does the HTTP. Field names and
// formats follow the user's pymetaverse login.py.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::session
{

inline constexpr std::string_view kAgniLoginUri =
    "https://login.agni.lindenlab.com/cgi-bin/login.cgi"; // main grid
inline constexpr std::string_view kAditiLoginUri =
    "https://login.aditi.lindenlab.com/cgi-bin/login.cgi"; // beta grid

// What the login server learns about this machine, in the official viewer's
// format (checked against lllogininstance.cpp, llhasheduniqueid.cpp,
// llmachineid.cpp, llappviewer{linux,win32,macosx}.cpp and llsys.cpp).
//
// WARNING: mac and id0 must describe the real machine. Falsifying them is a
// violation of the Second Life Terms of Service. Don't add a way to override
// them.
struct MachineInfo
{
    std::string platform;        // "Lnx", "Win", "Mac"
    std::string platformVersion; // dotted version: glibc on Linux, the OS elsewhere
    std::string platformString;  // OS name and version, e.g. "Linux 7.2"
    std::string mac;             // md5 hex of a 6-byte machine id (MAC address on Linux)
    std::string id0;             // md5 hex of a disk or hardware serial number
};

// Gathers the above. Missing ids come back as 32 zeros (mac) or the md5 of
// an empty string (id0), as the official viewer does.
MachineInfo queryMachineInfo();

// "$1$" + md5 of the password, as the login server expects. SL passwords are
// at most 16 characters, and the official viewer truncates to 16 before
// hashing, so we do too. Already-hashed input ("$1$...") is returned as is.
std::string hashPassword(std::string_view password);

struct AgentName
{
    std::string first;
    std::string last;
};

// "First Last", "first.last" or just "first" (last name becomes "Resident").
// Returns an error message if there's nothing usable.
std::expected<AgentName, std::string> parseAgentName(std::string_view username);

// Login options the server should include in its reply. These are enough for
// the session data we use today; inventory skeletons etc. come later.
std::vector<std::string> defaultLoginOptions();

struct LoginParameters
{
    std::string loginUri{kAgniLoginUri};
    AgentName name{};
    std::string passwordHash{}; // from hashPassword(); never the plain password
    // "last", "home", or "uri:<region>&<x>&<y>&<z>".
    std::string start = "last";

    std::string channel{};
    std::string version{};
    MachineInfo machine{};
    std::vector<std::string> options = defaultLoginOptions();

    // Set only after the user has seen and accepted what the server asked for
    // (reasons "tos" and "critical"); see LoginFailure.
    bool agreeToTos = false;
    bool readCritical = false;

    // Multi-factor authentication: the token the user typed after an
    // "mfa_challenge" failure, and the hash a previous login returned.
    std::string mfaToken{};
    std::string mfaHash{};
};

llsd::Value buildLoginRequest(const LoginParameters& parameters);

struct LoginSuccess
{
    AgentName name{};
    Uuid agentId{};
    Uuid sessionId{};
    Uuid secureSessionId{};
    uint32_t circuitCode = 0;
    std::string simIp{};
    uint16_t simPort = 0;
    uint32_t regionX = 0; // global position of the region's corner, in meters
    uint32_t regionY = 0;
    std::string seedCapability{};
    std::string message{}; // message of the day
    std::string mfaHash{}; // store to skip the MFA challenge next time
    llsd::Value reply{};   // the full reply, for fields not parsed above
};

struct LoginFailure
{
    // The server's reason ("key", "tos", "critical", "mfa_challenge",
    // "presence", "update", ...), or one of ours: "http" (no usable HTTP
    // response), "llsd" (unparseable reply), "incomplete" (success without
    // required fields), "indeterminate" (redirect logins; not supported yet).
    std::string reason{};
    std::string message{};
};

// Interprets the login server's LLSD reply.
std::expected<LoginSuccess, LoginFailure> parseLoginReply(const llsd::Value& reply);

} // namespace viewer::session
