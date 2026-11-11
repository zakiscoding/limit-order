#!/usr/bin/env bash
# Configure and build lob_engine on Linux or macOS.
#
#   scripts/build.sh [--instrument] [--sanitize] [--count-alloc] [--clean]
#
# Environment:
#   CXX          C++ compiler (default: whatever CMake finds; CI sets g++ or clang++)
#   BUILD_TYPE   CMake build type (default: Release)
#   BUILD_DIR    Output directory (default: cpp/build, cpp/build-prof with
#                --instrument, cpp/build-asan with --sanitize, cpp/build-alloc
#                with --count-alloc)
#   CMAKE_EXTRA  Extra arguments passed to the CMake configure step
#
# Prints the path of the built binary on the last line of stdout.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
instrument=OFF
sanitize=OFF
count_alloc=OFF
clean=0

for arg in "$@"; do
    case "$arg" in
        --instrument) instrument=ON ;;
        --sanitize) sanitize=ON ;;
        --count-alloc) count_alloc=ON ;;
        --clean) clean=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "build.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

default_dir="$ROOT/cpp/build"
[ "$instrument" = ON ] && default_dir="$ROOT/cpp/build-prof"
[ "$sanitize" = ON ] && default_dir="$ROOT/cpp/build-asan"
[ "$count_alloc" = ON ] && default_dir="$ROOT/cpp/build-alloc"
BUILD_DIR="${BUILD_DIR:-$default_dir}"

command -v cmake >/dev/null 2>&1 || { echo "build.sh: cmake not found" >&2; exit 1; }

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

compiler=()
if [ -n "${CXX:-}" ]; then
    compiler=(-DCMAKE_CXX_COMPILER="$CXX")
fi

[ "$clean" = 1 ] && rm -rf "$BUILD_DIR"

# shellcheck disable=SC2086  # CMAKE_EXTRA is intentionally word-split
cmake -S "$ROOT/cpp" -B "$BUILD_DIR" "${generator[@]}" "${compiler[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLOB_INSTRUMENT="$instrument" \
    -DLOB_SANITIZE="$sanitize" \
    -DLOB_COUNT_ALLOC="$count_alloc" \
    ${CMAKE_EXTRA:-} >&2
cmake --build "$BUILD_DIR" >&2

echo "$BUILD_DIR/lob_engine"
