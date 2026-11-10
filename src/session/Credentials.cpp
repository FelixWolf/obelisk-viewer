#include "viewer/session/Credentials.hpp"

#include "viewer/core/Log.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <vector>

namespace viewer::session
{

namespace
{

std::string trimmed(std::string s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if(first == std::string::npos)
    {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

void wipe(std::string& s)
{
    std::ranges::fill(s, '\0');
    s.clear();
}

} // namespace

std::expected<Credentials, std::string> readCredentialsFile(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if(ec || !fs::is_regular_file(status))
    {
        return std::unexpected(std::format("{}: not a readable file", path.string()));
    }
#if !defined(_WIN32)
    const auto others = fs::perms::group_read | fs::perms::others_read;
    if((status.permissions() & others) != fs::perms::none)
    {
        log::warn("{} is readable by other users; consider chmod 600", path.string());
    }
#endif

    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while(lines.size() < 3 && std::getline(in, line))
    {
        lines.push_back(trimmed(line));
        wipe(line);
    }
    auto wipeAll = [&]
    {
        for(auto& l : lines)
        {
            wipe(l);
        }
    };
    if(lines.size() < 2 || lines[1].empty())
    {
        wipeAll();
        return std::unexpected(
            std::format("{}: expected a username line and a password line", path.string()));
    }

    auto name = parseAgentName(lines[0]);
    if(!name)
    {
        wipeAll();
        return std::unexpected(std::format("{}: {}", path.string(), name.error()));
    }
    Credentials credentials;
    credentials.name = std::move(*name);
    credentials.passwordHash = hashPassword(lines[1]);
    credentials.loginUri =
        lines.size() > 2 && !lines[2].empty() ? lines[2] : std::string(kAditiLoginUri);
    wipeAll();
    return credentials;
}

} // namespace viewer::session
