#!/usr/bin/env bash
# Configures and builds one target/configuration through CMakePresets.json.
#
#   ./build.sh <linux|windows> <debug|release|dist> [extra cmake --build args]
#
#   ./build.sh linux debug          -> build/debug
#   ./build.sh windows release      -> build/windows-release (MinGW-w64)
#   ./build.sh linux release --target viewer_tests
set -euo pipefail

usage()
{
    echo "usage: $0 <linux|windows> <debug|release|dist> [extra cmake --build args]" >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
target=$1
config=$2
shift 2

case "$config" in
    debug|release|dist) ;;
    *) usage ;;
esac

case "$target" in
    linux)
        preset=$config
        ;;
    windows)
        preset=windows-$config
        if ! command -v x86_64-w64-mingw32-g++ >/dev/null; then
            echo "error: x86_64-w64-mingw32-g++ not found (install mingw-w64-gcc)" >&2
            exit 1
        fi
        ;;
    *) usage ;;
esac

cd "$(dirname "$0")"
cmake --preset "$preset"
cmake --build --preset "$preset" "$@"
echo "Built: build/$preset/bin"
