#include "viewer/media/UrlPolicy.hpp"

#include <array>
#include <charconv>
#include <optional>

namespace viewer::media
{

namespace
{

std::optional<std::array<int, 4>> ipv4(std::string_view host)
{
    std::array<int, 4> parts{};
    for(size_t i = 0; i < 4; i++)
    {
        const size_t dot = host.find('.');
        const std::string_view part = host.substr(0, dot);
        int v = -1;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), v);
        if(ec != std::errc() || end != part.data() + part.size() || v < 0 || v > 255 ||
           part.empty() || (i < 3) != (dot != std::string_view::npos))
        {
            return std::nullopt;
        }
        parts[i] = v;
        host = dot == std::string_view::npos ? std::string_view() : host.substr(dot + 1);
    }
    return parts;
}

bool localIpv4(const std::array<int, 4>& a)
{
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) ||
           (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
           (a[0] == 100 && a[1] >= 64 && a[1] <= 127) || a[0] >= 224;
}

// "[...]" as Chromium writes IPv6 hosts.
bool localIpv6(std::string_view host)
{
    std::string_view a = host.substr(1, host.size() - 2);
    if(a == "::" || a == "::1")
    {
        return true;
    }
    // An IPv4 address within (::ffff:a.b.c.d, ::a.b.c.d): Chromium writes
    // these as hex groups, ::ffff:7f00:1.
    if(a.starts_with("::ffff:") ||
       (a.starts_with("::") && a.find(':', 2) == std::string_view::npos))
    {
        return true; // only ever a way around the IPv4 test
    }
    const size_t colon = a.find(':');
    const std::string_view first = a.substr(0, colon);
    unsigned v = 0;
    const auto [end, ec] = std::from_chars(first.data(), first.data() + first.size(), v, 16);
    if(ec != std::errc() || first.empty())
    {
        return true; // not what Chromium writes: not trusted
    }
    // fc00::/7 unique local, fe80::/10 link-local, fec0::/10 site-local,
    // ff00::/8 multicast.
    return (v & 0xFE00) == 0xFC00 || (v & 0xFFC0) == 0xFE80 || (v & 0xFFC0) == 0xFEC0 ||
           (v & 0xFF00) == 0xFF00;
}

struct Parts
{
    std::string_view scheme;
    std::string_view host; // empty: none (data:, about:, blob:)
};

std::optional<Parts> parts(std::string_view url)
{
    const size_t colon = url.find(':');
    if(colon == std::string_view::npos || colon == 0)
    {
        return std::nullopt;
    }
    Parts p{url.substr(0, colon), {}};
    std::string_view rest = url.substr(colon + 1);
    if(rest.starts_with("//"))
    {
        rest.remove_prefix(2);
        std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
        if(const size_t at = authority.rfind('@'); at != std::string_view::npos)
        {
            authority.remove_prefix(at + 1);
        }
        if(authority.starts_with('['))
        {
            const size_t close = authority.find(']');
            if(close == std::string_view::npos)
            {
                return std::nullopt;
            }
            p.host = authority.substr(0, close + 1);
        }
        else
        {
            p.host = authority.substr(0, authority.find(':'));
        }
        if(p.host.empty())
        {
            return std::nullopt;
        }
    }
    return p;
}

bool allowed(std::string_view url, bool allowData, bool resource)
{
    const auto p = parts(url);
    if(!p)
    {
        return false;
    }
    if(p->scheme == "http" || p->scheme == "https" ||
       (resource && (p->scheme == "ws" || p->scheme == "wss")))
    {
        return !p->host.empty() && !localHost(p->host);
    }
    if(p->scheme == "about")
    {
        return url == "about:blank";
    }
    if(p->scheme == "data")
    {
        return allowData || resource; // a page's own images and fonts
    }
    if(p->scheme == "blob")
    {
        return resource; // made by a page: its origin's
    }
    return false;
}

} // namespace

bool localHost(std::string_view host)
{
    if(host.ends_with('.'))
    {
        host.remove_suffix(1);
    }
    if(host == "localhost" || host.ends_with(".localhost"))
    {
        return true;
    }
    if(host.starts_with('['))
    {
        return host.size() < 3 || !host.ends_with(']') || localIpv6(host);
    }
    if(const auto a = ipv4(host))
    {
        return localIpv4(*a);
    }
    return false;
}

bool navigable(std::string_view url, bool allowData)
{
    return allowed(url, allowData, false);
}

bool loadable(std::string_view url, bool allowData)
{
    return allowed(url, allowData, true);
}

} // namespace viewer::media
