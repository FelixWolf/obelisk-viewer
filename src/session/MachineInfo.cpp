// Machine identification for the login request, matching what the official
// viewer sends. Behavior checked against (not copied from):
//   mac: llhasheduniqueid.cpp, llmachineid.cpp, lluuid.cpp (getNodeID)
//   id0: llappviewerlinux.cpp / llappviewerwin32.cpp / llappviewermacosx.cpp
//        (generateSerialNumber)
//   platform strings: llsys.cpp (LLOSInfo)

#include "viewer/core/Log.hpp"
#include "viewer/core/Md5.hpp"
#include "viewer/session/Login.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <gnu/libc-version.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#endif

namespace viewer::session
{

namespace
{

using MachineId = std::array<uint8_t, 6>;

// Folds a serial-number string into 6 bytes by adding each character to
// byte (index % 6), as the official viewer does for hardware serials.
[[maybe_unused]] MachineId foldSerial(std::string_view serial)
{
    MachineId id{};
    for(size_t i = 0; i < serial.size(); i++)
    {
        id[i % id.size()] =
            static_cast<uint8_t>(id[i % id.size()] + static_cast<uint8_t>(serial[i]));
    }
    return id;
}

#if defined(_WIN32)

std::string platformName()
{
    return "Win";
}

std::optional<OSVERSIONINFOW> windowsVersion()
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
            return info;
        }
    }
    return std::nullopt;
}

std::string platformVersion()
{
    const auto v = windowsVersion();
    return v ? std::format("{}.{}.{}", v->dwMajorVersion, v->dwMinorVersion, v->dwBuildNumber)
             : "0.0.0";
}

std::string platformString()
{
    const auto v = windowsVersion();
    if(!v)
    {
        return "Unsupported Windows version ";
    }
    // Windows 11 still reports 10.0; build 22000 is where 11 starts.
    if(v->dwMajorVersion == 10 && v->dwBuildNumber >= 22000)
    {
        return "Microsoft Windows 11 ";
    }
    if(v->dwMajorVersion == 10)
    {
        return "Microsoft Windows 10 ";
    }
    return "Unsupported Windows version ";
}

std::optional<MachineId> machineId()
{
    // TODO: the official viewer uses the WMI Win32_ComputerSystemProduct UUID
    // (folded like a serial), falling back to the Windows product id. Until
    // that's implemented, use the first adapter's MAC, which is the source it
    // uses where neither exists. Must be done before Windows builds ship.
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    if(GetAdaptersInfo(adapters, &size) == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    }
    if(GetAdaptersInfo(adapters, &size) != NO_ERROR)
    {
        return std::nullopt;
    }
    for(auto* a = adapters; a; a = a->Next)
    {
        if(a->AddressLength == 6)
        {
            MachineId id{};
            std::copy_n(a->Address, 6, id.begin());
            return id;
        }
    }
    return std::nullopt;
}

std::string serialNumberHash()
{
    // The system drive's volume serial number, as 4 raw bytes.
    DWORD serial = 0;
    if(!GetVolumeInformationW(L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
    {
        log::warn("GetVolumeInformation failed; id0 will be empty");
        return {};
    }
    return toHex(md5(std::span(reinterpret_cast<const uint8_t*>(&serial), sizeof(serial))));
}

#elif defined(__APPLE__)

std::string platformName()
{
    return "Mac";
}

std::string macOsVersion()
{
    char version[64] = {};
    size_t size = sizeof(version);
    if(sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
    {
        return "0.0.0";
    }
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::sscanf(version, "%d.%d.%d", &major, &minor, &patch);
    return std::format("{}.{}.{}", major, minor, patch);
}

std::string platformVersion()
{
    return macOsVersion();
}

std::string platformString()
{
    // TODO: check the exact wording against llsys.cpp's Darwin branch.
    return "macOS " + macOsVersion();
}

std::optional<std::string> platformSerial()
{
    io_service_t expert = IOServiceGetMatchingService(kIOMainPortDefault,
                                                      IOServiceMatching("IOPlatformExpertDevice"));
    if(!expert)
    {
        return std::nullopt;
    }
    auto serial = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(
        expert, CFSTR(kIOPlatformSerialNumberKey), kCFAllocatorDefault, 0));
    IOObjectRelease(expert);
    if(!serial)
    {
        return std::nullopt;
    }
    char buffer[256] = {};
    const bool ok = CFStringGetCString(serial, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(serial);
    if(!ok || buffer[0] == 0)
    {
        return std::nullopt;
    }
    return std::string(buffer);
}

std::optional<MachineId> machineId()
{
    // The platform serial number folded into 6 bytes. (The official viewer
    // falls back to the MAC address; not implemented yet.)
    if(auto serial = platformSerial())
    {
        return foldSerial(*serial);
    }
    return std::nullopt;
}

std::string serialNumberHash()
{
    const auto serial = platformSerial();
    return serial ? md5Hex(*serial) : std::string();
}

#else // Linux

std::string platformName()
{
    return "Lnx";
}

std::string platformVersion()
{
    // The glibc version stands in for the OS version, as major.minor.build.
    int major = 0;
    int minor = 0;
    int build = 0;
    std::sscanf(gnu_get_libc_version(), "%d.%d.%d", &major, &minor, &build);
    return std::format("{}.{}.{}", major, minor, build);
}

std::string platformString()
{
    utsname u{};
    if(uname(&u) != 0)
    {
        return "Unable to collect OS info";
    }
    std::string simple = std::format("{} {}", u.sysname, u.release);
    if(std::string_view(u.sysname) == "Linux")
    {
        // Major and minor kernel version only: cut at the second dot.
        const size_t first = simple.find('.');
        const size_t second = first == std::string::npos ? first : simple.find('.', first + 1);
        simple = simple.substr(0, second);
    }
    return simple;
}

std::optional<MachineId> readMac(const std::string& interfaceName)
{
    std::ifstream in("/sys/class/net/" + interfaceName + "/address");
    std::string text;
    if(!std::getline(in, text))
    {
        return std::nullopt;
    }
    unsigned int b[6] = {};
    if(std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) !=
       6)
    {
        return std::nullopt;
    }
    MachineId id{};
    for(size_t i = 0; i < 6; i++)
    {
        id[i] = static_cast<uint8_t>(b[i]);
    }
    return id;
}

std::optional<MachineId> machineId()
{
    // The hardware address of the first interface that has an IPv4 address
    // and a non-zero hardware address (loopback's is all zeros), in the
    // kernel's interface order.
    ifaddrs* list = nullptr;
    if(getifaddrs(&list) != 0)
    {
        return std::nullopt;
    }
    std::optional<MachineId> result;
    for(ifaddrs* i = list; i && !result; i = i->ifa_next)
    {
        if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if(auto id = readMac(i->ifa_name); id && *id != MachineId{})
        {
            result = id;
        }
    }
    freeifaddrs(list);
    return result;
}

std::string serialNumberHash()
{
    // The longest (then alphabetically last) filesystem UUID; empty if none.
    namespace fs = std::filesystem;
    std::string best;
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator("/dev/disk/by-uuid", ec))
    {
        const std::string name = entry.path().filename().string();
        if(name.size() > best.size() || (name.size() == best.size() && name > best))
        {
            best = name;
        }
    }
    return md5Hex(best);
}

#endif

} // namespace

MachineInfo queryMachineInfo()
{
    MachineInfo info;
    info.platform = platformName();
    info.platformVersion = platformVersion();
    info.platformString = platformString();

    if(const auto id = machineId())
    {
        info.mac = toHex(md5(std::span<const uint8_t>(*id)));
    }
    else
    {
        // Report the truth (no id) rather than inventing one.
        log::warn("couldn't find a unique machine id; sending zeros");
        info.mac = std::string(32, '0');
    }
    info.id0 = serialNumberHash();
    if(info.id0.empty())
    {
        info.id0 = md5Hex("");
    }
    return info;
}

} // namespace viewer::session
