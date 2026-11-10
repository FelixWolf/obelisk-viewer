#include "viewer/core/Log.hpp"
#include "viewer/core/Md5.hpp"
#include "viewer/session/Login.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <iphlpapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#endif

namespace viewer::session
{

namespace
{

using MacBytes = std::array<uint8_t, 6>;

struct Candidate
{
    std::string name;
    MacBytes mac{};
    bool physical = false;
};

bool isUsable(const MacBytes& mac)
{
    bool allZero = true;
    for(const uint8_t b : mac)
    {
        if(b != 0)
        {
            allZero = false;
        }
    }
    const bool multicast = (mac[0] & 0x01) != 0;
    return !allZero && !multicast;
}

// Prefers physical devices, then universally administered addresses (the ones
// burned in by the manufacturer), then interface name order.
std::optional<MacBytes> pickInterface(std::vector<Candidate> candidates)
{
    std::erase_if(candidates, [](const Candidate& c) { return !isUsable(c.mac); });
    if(candidates.empty())
    {
        return std::nullopt;
    }
    auto rank = [](const Candidate& c)
    {
        const bool universal = (c.mac[0] & 0x02) == 0;
        return std::tuple(!c.physical, !universal, c.name);
    };
    std::ranges::sort(candidates,
                      [&](const Candidate& a, const Candidate& b) { return rank(a) < rank(b); });
    return candidates.front().mac;
}

#if defined(_WIN32)

std::string platformName()
{
    return "Win";
}

std::string platformVersion()
{
    // GetVersionEx lies to unmanifested programs; RtlGetVersion doesn't.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if(HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        auto rtlGetVersion =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if(rtlGetVersion && rtlGetVersion(&info) == 0)
        {
            return std::format("Microsoft Windows {}.{}.{}", info.dwMajorVersion,
                               info.dwMinorVersion, info.dwBuildNumber);
        }
    }
    return "Microsoft Windows";
}

std::vector<Candidate> interfaces()
{
    std::vector<Candidate> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG status = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &size);
    if(status == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &size);
    }
    if(status != NO_ERROR)
    {
        return result;
    }
    for(auto* a = addresses; a; a = a->Next)
    {
        if(a->PhysicalAddressLength != 6 || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }
        Candidate c;
        c.name = a->AdapterName;
        std::copy_n(a->PhysicalAddress, 6, c.mac.begin());
        c.physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
        result.push_back(std::move(c));
    }
    return result;
}

#elif defined(__APPLE__)

std::string platformName()
{
    return "Mac";
}

std::string platformVersion()
{
    utsname u{};
    if(uname(&u) != 0)
    {
        return "macOS";
    }
    return std::format("{} {}", u.sysname, u.release);
}

std::vector<Candidate> interfaces()
{
    std::vector<Candidate> result;
    ifaddrs* list = nullptr;
    if(getifaddrs(&list) != 0)
    {
        return result;
    }
    for(ifaddrs* i = list; i; i = i->ifa_next)
    {
        if(!i->ifa_addr || i->ifa_addr->sa_family != AF_LINK)
        {
            continue;
        }
        const auto* link = reinterpret_cast<const sockaddr_dl*>(i->ifa_addr);
        if(link->sdl_alen != 6 || link->sdl_type != IFT_ETHER)
        {
            continue;
        }
        Candidate c;
        c.name = i->ifa_name;
        const auto* bytes = reinterpret_cast<const uint8_t*>(LLADDR(link));
        std::copy_n(bytes, 6, c.mac.begin());
        c.physical = c.name.starts_with("en");
        result.push_back(std::move(c));
    }
    freeifaddrs(list);
    return result;
}

#else // Linux and other POSIX

std::string platformName()
{
    return "Lnx";
}

std::string platformVersion()
{
    utsname u{};
    if(uname(&u) != 0)
    {
        return "Linux";
    }
    return std::format("{} {}", u.sysname, u.release);
}

std::optional<MacBytes> parseMac(const std::string& text)
{
    MacBytes mac{};
    unsigned int b[6] = {};
    if(std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) !=
       6)
    {
        return std::nullopt;
    }
    for(size_t i = 0; i < 6; i++)
    {
        mac[i] = static_cast<uint8_t>(b[i]);
    }
    return mac;
}

std::vector<Candidate> interfaces()
{
    namespace fs = std::filesystem;
    std::vector<Candidate> result;
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator("/sys/class/net", ec))
    {
        Candidate c;
        c.name = entry.path().filename().string();
        if(c.name == "lo")
        {
            continue;
        }
        std::ifstream in(entry.path() / "address");
        std::string text;
        if(!std::getline(in, text))
        {
            continue;
        }
        const auto mac = parseMac(text);
        if(!mac)
        {
            continue;
        }
        c.mac = *mac;
        // Virtual interfaces (bridges, veth, docker, tun) have no device link.
        c.physical = fs::exists(entry.path() / "device", ec);
        result.push_back(std::move(c));
    }
    return result;
}

#endif

} // namespace

MachineInfo queryMachineInfo()
{
    MachineInfo info;
    info.platform = platformName();
    info.platformVersion = platformVersion();

    const auto mac = pickInterface(interfaces());
    if(!mac)
    {
        // Report the truth (no MAC found) rather than inventing one.
        log::warn("no network interface with a MAC address found; sending zeros");
    }
    const MacBytes bytes = mac.value_or(MacBytes{});
    info.mac = std::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", bytes[0], bytes[1],
                           bytes[2], bytes[3], bytes[4], bytes[5]);

    // WARNING: deviating from this format may violate the Terms of Service.
    info.id0 = md5Hex(std::format("{}:{}:{}", info.platform, info.mac, info.platformVersion));
    return info;
}

} // namespace viewer::session
