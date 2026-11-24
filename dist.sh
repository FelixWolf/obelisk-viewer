#!/usr/bin/env bash
# Builds a distributable package with Velopack (docs/packaging.md).
#
#   ./dist.sh <linux|windows> <release|debug> [extra cmake configure args]
#
#   ./dist.sh windows release  -> Setup.exe, portable zip, update packages
#   ./dist.sh linux release    -> AppImage and update packages
#   ./dist.sh linux release -DVIEWER_NAME="My Viewer"
#
# The names and version come from CMake (cmake/Branding.cmake, project()).
#
# release: the dist preset (Release, no testing options, stripped).
# debug:   the debug preset (console on Windows, testing options, symbols),
#          packed as a separate app (<package id>-Debug) so it installs beside
#          the release one and never updates into it.
#
# Each builds in its own folder, build/package-<os>-<config>, with
# VIEWER_VELOPACK on; the everyday build/<preset> folders are untouched.
# Output: build/package-<os>-<config>/releases. Needs vpk
# (dotnet tool install -g vpk --version 1.2.161, matching the library).
set -euo pipefail

usage()
{
    echo "usage: $0 <linux|windows> <release|debug> [extra cmake configure args]" >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
os=$1
config=$2
shift 2

case "$config" in
    release) preset=dist ;;
    debug) preset=debug ;;
    *) usage ;;
esac

case "$os" in
    linux)
        exe_suffix=
        runtime=linux-x64
        vpk_directive=()
        ;;
    windows)
        preset=windows-$preset
        exe_suffix=.exe
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

build_dir=build/package-$os-$config
stage=$build_dir/stage
releases=$build_dir/releases

cmake --preset "$preset" -B "$build_dir" -DVIEWER_VELOPACK=ON "$@"

# A value the configure step wrote (cmake/Branding.cmake).
branding()
{
    sed -n "s/^$1=//p" "$build_dir/branding.txt"
}

version=$(branding version)
pack_id=$(branding package_id)
pack_title=$(branding name)
main_exe=$(branding executable)$exe_suffix
if [[ -z "$version" || -z "$pack_id" || -z "$pack_title" || "$main_exe" == "$exe_suffix" ]]; then
    echo "error: $build_dir/branding.txt is missing something" >&2
    exit 1
fi

if [[ $config == release ]]; then
    strip=(--strip)
else
    pack_id+=-Debug
    pack_title+=" (Debug)"
    strip=()
fi

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
