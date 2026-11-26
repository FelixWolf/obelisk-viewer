#include "MediaWhitelist.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace viewer
{

namespace
{

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

// Lowercase labels of letters, digits and hyphens, dots between.
bool isHost(std::string_view host)
{
    if(host.empty() || host.front() == '.' || host.back() == '.' ||
       host.find("..") != std::string_view::npos)
    {
        return false;
    }
    return std::ranges::all_of(
        host, [](char c)
        { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; });
}

struct Place
{
    std::string host;
    std::string path;
};

// Nothing if it isn't plainly an https address.
std::optional<Place> placeOf(std::string_view url)
{
    constexpr std::string_view kScheme = "https://";
    if(url.size() <= kScheme.size() || lowered(url.substr(0, kScheme.size())) != kScheme)
    {
        return std::nullopt;
    }
    // Spaces, controls and what isn't ASCII: browsers drop or convert some.
    // A backslash is a slash to them.
    if(std::ranges::any_of(url, [](char c) { return c <= ' ' || c >= 0x7F || c == '\\'; }))
    {
        return std::nullopt;
    }
    std::string_view rest = url.substr(kScheme.size());
    rest = rest.substr(0, rest.find_first_of("?#"));
    const size_t slash = rest.find('/');
    std::string host = lowered(rest.substr(0, slash));
    if(host.ends_with(":443"))
    {
        host.resize(host.size() - 4);
    }
    if(host.ends_with('.'))
    {
        host.pop_back(); // the same host, said in full
    }
    if(!isHost(host)) // a user name or another port too
    {
        return std::nullopt;
    }
    return Place{std::move(host),
                 slash == std::string_view::npos ? "/" : std::string(rest.substr(slash))};
}

// Whether a path may be read as another: "." and ".." parts, or escapes.
bool isPlainPath(std::string_view path)
{
    if(path.find('%') != std::string_view::npos)
    {
        return false;
    }
    while(!path.empty())
    {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if(part == "." || part == "..")
        {
            return false;
        }
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    }
    return true;
}

bool hostMatches(const MediaSite& site, std::string_view host)
{
    if(host == site.host)
    {
        return true;
    }
    return site.subdomains && host.size() > site.host.size() && host.ends_with(site.host) &&
           host[host.size() - site.host.size() - 1] == '.';
}

bool pathMatches(const MediaSite& site, std::string_view path)
{
    if(site.path == "/")
    {
        return true;
    }
    std::string_view under = site.path;
    if(under.ends_with('/'))
    {
        under.remove_suffix(1);
    }
    return isPlainPath(path) && path.starts_with(under) &&
           (path.size() == under.size() || path[under.size()] == '/');
}

} // namespace

bool MediaWhitelist::allows(std::string_view url) const
{
    const auto place = placeOf(url);
    return place && std::ranges::any_of(
                        sites, [&](const MediaSite& s)
                        { return hostMatches(s, place->host) && pathMatches(s, place->path); });
}

std::expected<MediaWhitelist, std::string> parseMediaWhitelist(std::string_view json)
{
    const auto root = llsd::fromJson(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    if(!root->isMap() || (*root)["schema"].asInteger() != 1)
    {
        return std::unexpected("not schema 1");
    }
    const llsd::Value& sites = (*root)["sites"];
    if(!sites.isArray())
    {
        return std::unexpected("no sites");
    }
    MediaWhitelist list;
    for(size_t i = 0; i < sites.size(); i++)
    {
        const llsd::Value& s = sites.at(i);
        MediaSite site;
        site.host = lowered(s["host"].asString());
        site.subdomains = s["subdomains"].asBoolean();
        site.path = s["path"].asString();
        // One we can't make sense of is left out rather than guessed at (a
        // host without a dot would list a whole top-level domain).
        if(isHost(site.host) && site.host.find('.') != std::string::npos &&
           site.path.starts_with('/') && isPlainPath(site.path))
        {
            list.sites.push_back(std::move(site));
        }
    }
    if(const int refresh = (*root)["refresh"].asInteger(); refresh > 0)
    {
        list.refresh = refresh;
    }
    return list;
}

MediaPlay mediaPlayFrom(std::string_view setting)
{
    if(setting == "always")
    {
        return MediaPlay::Always;
    }
    if(setting == "click")
    {
        return MediaPlay::Click;
    }
    if(setting == "disable")
    {
        return MediaPlay::Disable;
    }
    return MediaPlay::Whitelist;
}

MediaStart mediaStart(MediaPlay play, bool autoPlay, std::string_view url,
                      const MediaWhitelist& whitelist)
{
    switch(play)
    {
    case MediaPlay::Disable:
        return MediaStart::Never;
    case MediaPlay::Click:
        return MediaStart::Click;
    case MediaPlay::Always:
        return autoPlay ? MediaStart::Play : MediaStart::Click;
    case MediaPlay::Whitelist:
        break;
    }
    return autoPlay && whitelist.allows(url) ? MediaStart::Play : MediaStart::Click;
}

} // namespace viewer
