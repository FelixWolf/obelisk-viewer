# Packaging

The `dist` preset is the build that ships: `Release` (no `VIEWER_TEST_OPTIONS`,
so no `--login-file`, `--screenshot`, ...), with no developer tools or tests.

```sh
cmake --preset dist
cmake --build --preset dist
cmake --install build/dist --component viewer --prefix build/dist/stage --strip
```

(`./build.sh linux dist` does the first two.) For Windows from Linux,
`./build.sh windows dist`, then the same install from `build/windows-dist`.

`build/dist/stage` then holds only what ships, laid out the way the viewer
expects (it finds `assets/` beside itself):

```
viewer(.exe)
openxr_loader.dll                   (Windows: the OpenXR loader SDL opens for --vr)
assets/character/  assets/ui/  assets/home.scene
LICENSE  LICENSE-artwork.txt  LICENSE-Inter.txt
```

Only the `viewer` install component: the dependencies' own install rules
(static libraries, headers) and the generator `viewer_msggen` stay out.
Nothing per-user is written beside the program: settings, saved logins and
ImGui's window layout (`imgui.ini`) go to the user's settings folder
(`~/.config/viewer`, `%APPDATA%\viewer`), the cache to the user's cache folder.

## Zip

`cpack` in `build/dist` makes `build/dist/package/TestViewer-<version>-<OS>.zip`
from the same component.

## Velopack

```sh
./dist.sh <linux|windows> <release|debug>
```

builds and packs one installer into `build/package-<os>-<config>/releases`:
on Windows `TestViewer-win-Setup.exe`, `TestViewer-win-Portable.zip` and the
update packages (`.nupkg`, `releases.win.json`); on Linux an AppImage and
the same. It builds in its own folder, `build/package-<os>-<config>`, with
`VIEWER_VELOPACK` on, then installs the `viewer` component (above) into
`stage/` there and runs `vpk pack` on it (`vpk [win] pack` from Linux). The
version is `project()`'s in `CMakeLists.txt`; Velopack only updates to a
higher one, so raise it for every release.

- `release`: the `dist` preset, stripped. Pack id `TestViewer`.
- `debug`: the `debug` preset (a console on Windows, the testing options,
  symbols). Pack id `TestViewer-Debug`, so it installs beside the release
  one and neither updates into the other.

`VIEWER_VELOPACK` (off in everyday builds) downloads Velopack's prebuilt C
library (`cmake/Dependencies.cmake`, pinned with its hash), links it and
defines `VIEWER_VELOPACK`, so `main()` calls
`Velopack::VelopackApp::Build().Run()` first thing: the installer and
updater start the viewer with their own arguments to run its hooks, and it
exits there. On Windows the library is a DLL, installed beside
`viewer.exe` as `velopack_libc.dll`, the name its import library links
against (the zip's `velopack_libc_win_x64_msvc.dll`; MSVC-built,
but a C interface, so the MinGW build links it; it needs only system DLLs).
On Linux it is linked statically.

`vpk` is a .NET tool, its version matching the library's:

```sh
dotnet tool install -g vpk --version 1.2.161
```

Still to do: nothing checks for updates yet (Velopack's `UpdateManager`,
pointed at wherever the releases folder is published); the packages aren't
signed (`vpk pack --signTemplate`, e.g. with `osslsigncode`), so Windows
SmartScreen warns about `Setup.exe`; there is no icon (`--icon`, an `.ico`).
