#include "viewer/xr/SteamVr.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace viewer::xr
{

// OpenVR's internal client interface, as far as it's used here: what
// openvr_api itself gets from vrclient ("IVRClientCore_003"). Only the order
// of the functions matters. Not in the unnamed namespace: the compiler would
// know nothing here implements it, and call the pure virtual functions directly.
class ClientCore
{
public:
    virtual int init(int applicationType, const char* startupInfo) = 0;
    virtual void cleanup() = 0;
    virtual int isInterfaceVersionValid(const char* version) = 0;
    virtual void* getGenericInterface(const char* version, int* error) = 0;
    virtual bool isHmdPresent() = 0;

protected:
    ~ClientCore() = default; // SteamVR's, never deleted
};

namespace
{

namespace fs = std::filesystem;

using ClientCoreFactory = void* (*)(const char* name, int* error);

// Where SteamVR lists the places it's installed.
fs::path pathRegistry()
{
#if defined(_WIN32)
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return local && *local ? fs::path(local) / "openvr" / "openvrpaths.vrpath" : fs::path();
#else
    if(const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / "openvr" / "openvrpaths.vrpath";
    }
    const char* home = std::getenv("HOME");
    return home && *home ? fs::path(home) / ".config" / "openvr" / "openvrpaths.vrpath"
                         : fs::path();
#endif
}

// The strings in the file's "runtime" list. The file is JSON, UTF-8, written
// by SteamVR; backslashes are the only escapes its paths need.
std::vector<fs::path> runtimes()
{
    std::ifstream file(pathRegistry(), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    std::vector<fs::path> paths;
    const size_t key = text.find("\"runtime\"");
    const size_t open = key == std::string::npos ? key : text.find('[', key);
    if(open == std::string::npos)
    {
        return paths;
    }
    bool quoted = false;
    std::u8string path;
    for(size_t i = open + 1; i < text.size(); i++)
    {
        const char c = text[i];
        if(!quoted)
        {
            if(c == ']')
            {
                break;
            }
            quoted = c == '"';
        }
        else if(c == '"')
        {
            paths.emplace_back(path);
            path.clear();
            quoted = false;
        }
        else if(c == '\\' && i + 1 < text.size())
        {
            path += static_cast<char8_t>(text[++i]);
        }
        else
        {
            path += static_cast<char8_t>(c);
        }
    }
    return paths;
}

// Left loaded: the OpenXR runtime may be the same library.
ClientCoreFactory loadFactory(const fs::path& library)
{
    static constexpr const char* kFactory = "VRClientCoreFactory";
#if defined(_WIN32)
    // What it needs is looked for beside it, as openvr_api loads it.
    HMODULE module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)
    {
        log::warn("SteamVR: vrclient didn't load ({})", GetLastError());
        return nullptr;
    }
    return reinterpret_cast<ClientCoreFactory>(
        reinterpret_cast<void*>(GetProcAddress(module, kFactory)));
#else
    SDL_SharedObject* module = SDL_LoadObject(library.c_str());
    if(!module)
    {
        log::warn("SteamVR: {}", SDL_GetError());
        return nullptr;
    }
    return reinterpret_cast<ClientCoreFactory>(SDL_LoadFunction(module, kFactory));
#endif
}

} // namespace

bool steamVrHeadsetPresent()
{
#if defined(_WIN32)
    static constexpr const char* kLibrary = "bin/vrclient_x64.dll";
#else
    static constexpr const char* kLibrary = "bin/linux64/vrclient.so";
#endif
    for(const fs::path& runtime : runtimes())
    {
        fs::path library = runtime / kLibrary;
        library.make_preferred();
        std::error_code error;
        if(!library.is_absolute() || !fs::exists(library, error))
        {
            continue; // uninstalled, or moved
        }
        const ClientCoreFactory factory = loadFactory(library);
        int initError = 0;
        auto* core =
            factory ? static_cast<ClientCore*>(factory("IVRClientCore_003", &initError)) : nullptr;
        if(!core)
        {
            log::warn("SteamVR: can't ask it for a headset ({})", initError);
            return false;
        }
        const bool present = core->isHmdPresent();
        log::info("SteamVR: {}", present ? "headset found" : "no headset");
        return present;
    }
    log::info("SteamVR: not installed");
    return false;
}

} // namespace viewer::xr
