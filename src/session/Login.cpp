#include "viewer/session/Login.hpp"

#include "viewer/core/Md5.hpp"

#include <format>

namespace viewer::session
{

namespace
{

std::string_view trim(std::string_view s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if(first == std::string_view::npos)
    {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// The login server wraps first_name in literal double quotes.
std::string unquote(std::string s)
{
    if(s.size() >= 2 && s.front() == '"' && s.back() == '"')
    {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

} // namespace

std::string hashPassword(std::string_view password)
{
    if(password.starts_with("$1$"))
    {
        return std::string(password);
    }
    return "$1$" + md5Hex(password.substr(0, 16));
}

std::expected<AgentName, std::string> parseAgentName(std::string_view username)
{
    username = trim(username);
    if(username.empty())
    {
        return std::unexpected("Enter a username.");
    }

    AgentName name;
    const size_t split = username.find_first_of(". ");
    if(split == std::string_view::npos)
    {
        name.first = std::string(username);
        name.last = "Resident";
        return name;
    }
    name.first = std::string(trim(username.substr(0, split)));
    name.last = std::string(trim(username.substr(split + 1)));
    if(name.first.empty())
    {
        return std::unexpected("The username has no first name.");
    }
    if(name.last.empty())
    {
        name.last = "Resident";
    }
    if(name.last.find_first_of(". ") != std::string::npos)
    {
        return std::unexpected("A username has at most two parts.");
    }
    return name;
}

std::vector<std::string> defaultLoginOptions()
{
    // pymetaverse's OPTIONS_MOST.
    return {
        "inventory-root",  "inventory-lib-root", "inventory-lib-owner", "display_names",
        "adult_compliant", "advanced-mode",      "max_groups",          "max-agent-groups",
        "map-server-url",  "login-flags",
    };
}

llsd::Value buildLoginRequest(const LoginParameters& p)
{
    llsd::Array options;
    for(const auto& option : p.options)
    {
        options.emplace_back(option);
    }

    llsd::Value request;
    // Credentials
    request["first"] = p.name.first;
    request["last"] = p.name.last;
    request["passwd"] = p.passwordHash;
    // OS
    request["platform"] = p.machine.platform;
    request["platform_version"] = p.machine.platformVersion;
    request["platform_string"] = p.machine.platformString;
    request["address_size"] = static_cast<int32_t>(sizeof(void*) * 8);
    // Viewer
    request["channel"] = p.channel;
    request["version"] = p.version;
    // Machine. WARNING: falsifying mac or id0 violates the Terms of Service.
    request["host_id"] = "";
    request["mac"] = p.machine.mac;
    request["id0"] = p.machine.id0;
    // Agreements: only true once the user has actually accepted.
    request["skipoptional"] = true;
    request["agree_to_tos"] = p.agreeToTos;
    request["read_critical"] = p.readCritical;
    // Options
    request["extended_errors"] = true;
    request["options"] = std::move(options);
    request["agent_flags"] = 0; // bit 2 would mark us as a bot; we are not one
    request["start"] = p.start;
    request["last_exec_event"] = 0;
    // Multi-factor authentication
    request["token"] = p.mfaToken;
    request["mfa_hash"] = p.mfaHash;
    return request;
}

std::expected<LoginSuccess, LoginFailure> parseLoginReply(const llsd::Value& reply)
{
    if(!reply.isMap())
    {
        return std::unexpected(LoginFailure{"llsd", "The login server's reply isn't an LLSD map."});
    }

    const llsd::Value& login = reply["login"];
    const std::string loginText = login.type() == llsd::Value::Type::Boolean
                                      ? (login.asBoolean() ? "true" : "false")
                                      : login.asString();
    if(loginText == "indeterminate")
    {
        return std::unexpected(LoginFailure{
            "indeterminate", "The login server asked us to continue at another address, which "
                             "isn't supported yet."});
    }
    if(loginText != "true")
    {
        LoginFailure failure{reply["reason"].asString(), reply["message"].asString()};
        if(failure.reason.empty())
        {
            failure.reason = "unknown";
        }
        if(failure.message.empty())
        {
            failure.message = "Login failed.";
        }
        return std::unexpected(std::move(failure));
    }

    constexpr std::string_view kRequired[] = {"agent_id",       "session_id", "secure_session_id",
                                              "circuit_code",   "sim_ip",     "sim_port",
                                              "seed_capability"};
    std::string missing;
    for(const auto key : kRequired)
    {
        if(!reply.has(key))
        {
            missing += missing.empty() ? "" : ", ";
            missing += key;
        }
    }
    if(!missing.empty())
    {
        return std::unexpected(
            LoginFailure{"incomplete", std::format("The login reply is missing: {}.", missing)});
    }

    LoginSuccess success;
    success.name.first = unquote(reply["first_name"].asString());
    success.name.last = unquote(reply["last_name"].asString());
    success.agentId = reply["agent_id"].asUuid();
    success.sessionId = reply["session_id"].asUuid();
    success.secureSessionId = reply["secure_session_id"].asUuid();
    success.circuitCode = static_cast<uint32_t>(reply["circuit_code"].asInteger());
    success.simIp = reply["sim_ip"].asString();
    success.simPort = static_cast<uint16_t>(reply["sim_port"].asInteger());
    success.regionX = static_cast<uint32_t>(reply["region_x"].asInteger());
    success.regionY = static_cast<uint32_t>(reply["region_y"].asInteger());
    success.seedCapability = reply["seed_capability"].asString();
    success.message = reply["message"].asString();
    success.mfaHash = reply["mfa_hash"].asString();
    success.reply = reply;

    if(success.agentId.isNull() || success.sessionId.isNull() || success.seedCapability.empty())
    {
        return std::unexpected(LoginFailure{
            "incomplete", "The login reply has an empty agent id, session id or seed capability."});
    }
    return success;
}

} // namespace viewer::session
