#include "App.hpp"
#include "Updater.hpp"

#include "viewer/core/Branding.hpp"

#include <SDL3/SDL_main.h>

#if defined(VIEWER_VELOPACK)
#include <Velopack.hpp>
#endif

#if defined(_WIN32)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{

#if defined(_WIN32)
// Release builds are GUI programs (see src/app/CMakeLists.txt): no console,
// so the log (stderr) would go nowhere. Send it to
// %LOCALAPPDATA%\<kDirectory>\<kExecutable>.log instead, keeping the run before as
// <kExecutable>.old.log (the one to read after a crash). When stderr already goes
// somewhere (Debug builds' console, `<kExecutable>.exe 2> file`) it stays there.
void logToFileWithoutConsole()
{
    namespace fs = std::filesystem;
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if(handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN)
    {
        return;
    }
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if(local == nullptr || *local == L'\0')
    {
        return;
    }
    const fs::path directory = fs::path(local) / viewer::branding::kDirectory;
    const std::string name = viewer::branding::kExecutable;
    const fs::path file = directory / (name + ".log");
    std::error_code error;
    fs::create_directories(directory, error);
    fs::rename(file, directory / (name + ".old.log"), error); // none the first time
    if(_wfreopen(file.c_str(), L"w", stderr) != nullptr)
    {
        // Unbuffered: a crash loses no lines.
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}
#endif

} // namespace

int main(int argc, char** argv)
{
#if defined(VIEWER_VELOPACK)
    // Packaged builds (./dist.sh): the installer and updater start the viewer
    // with their own arguments to run these hooks, and it exits here then.
    // Also applies an update downloaded last time before going on.
    // Not with another viewer running, which applying it would end (Windows).
    Velopack::VelopackApp::Build()
        .SetAutoApplyOnStartup(!viewer::anotherInstanceRunning())
        .Run();
#endif
#if defined(_WIN32)
    logToFileWithoutConsole();
#endif
    // The settings (settings.llsd) first: the command line's --setting
    // overrides them for this run.
    auto settings = std::make_unique<viewer::Settings>(viewer::viewerSettings(),
                                                       viewer::Settings::defaultFile());
    auto options = viewer::parseArguments({argv, static_cast<size_t>(argc)}, *settings);
    if(!options)
    {
        return 2;
    }
    options->arguments.assign(argv + 1, argv + argc);
    auto app = viewer::App::create(*options, std::move(settings));
    if(!app)
    {
        return 1;
    }
    return app->run();
}
