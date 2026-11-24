#!/usr/bin/env bash
# Builds a distributable package with Velopack (docs/packaging.md).
#
#   ./dist.sh <linux|windows> <release|debug>
#
#   ./dist.sh windows release  -> Setup.exe, portable zip, update packages
#   ./dist.sh linux release    -> AppImage and update packages
#
# release: the dist preset (Release, no testing options, stripped).
# debug:   the debug preset (console on Windows, testing options, symbols),
#          packed as a separate app (TestViewer-Debug) so it installs beside
#          the release one and never updates into it.
#
# Each builds in its own folder, build/package-<os>-<config>, with
# VIEWER_VELOPACK on; the everyday build/<preset> folders are untouched.
# Output: build/package-<os>-<config>/releases. Needs vpk
# (dotnet tool install -g vpk --version 1.2.161, matching the library).
set -euo pipefail

usage()
{
    echo "usage: $0 <linux|windows> <release|debug>" >&2
    exit 2
}

[[ $# -eq 2 ]] || usage
os=$1
config=$2

case "$config" in
    release) preset=dist ;;
    debug) preset=debug ;;
    *) usage ;;
esac

case "$os" in
    linux)
        main_exe=viewer
        runtime=linux-x64
        vpk_directive=()
        ;;
    windows)
        preset=windows-$preset
        main_exe=viewer.exe
        runtime=win-x64
        vpk_directive=("[win]")
        if ! command -v x86_64-w64-mingw32-g++ >/dev/null; then
            echo "error: x86_64-w64-mingw32-g++ not found (install mingw-w64-gcc)" >&2
            exit 1
        fi
        ;;
    *) usage ;;
esac

vpk=$(command -v vpk || true)
if [[ -z "$vpk" && -x "$HOME/.dotnet/tools/vpk" ]]; then
    vpk=$HOME/.dotnet/tools/vpk
fi
if [[ -z "$vpk" ]]; then
    echo "error: vpk not found (dotnet tool install -g vpk --version 1.2.161)" >&2
    exit 1
fi

cd "$(dirname "$0")"

version=$(sed -nE '/^project\(/,/\)/{s/^[[:space:]]*VERSION[[:space:]]+([0-9.]+).*/\1/p}' CMakeLists.txt)
if [[ -z "$version" ]]; then
    echo "error: no VERSION in CMakeLists.txt's project()" >&2
    exit 1
fi

if [[ $config == release ]]; then
    pack_id=TestViewer
    pack_title=TestViewer
    strip=(--strip)
else
    pack_id=TestViewer-Debug
    pack_title="TestViewer (Debug)"
    strip=()
fi

build_dir=build/package-$os-$config
stage=$build_dir/stage
releases=$build_dir/releases

cmake --preset "$preset" -B "$build_dir" -DVIEWER_VELOPACK=ON
cmake --build "$build_dir" --target viewer

# Only what ships (the viewer component), from a clean folder.
rm -rf "$stage"
cmake --install "$build_dir" --component viewer --prefix "$stage" "${strip[@]}"

"$vpk" "${vpk_directive[@]}" pack \
    --packId "$pack_id" \
    --packVersion "$version" \
    --packTitle "$pack_title" \
    --packDir "$stage" \
    --runtime "$runtime" \
    --mainExe "$main_exe" \
    --outputDir "$releases"

echo "Packaged $pack_title $version ($os): $releases"
