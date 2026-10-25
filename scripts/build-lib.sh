#!/bin/bash
# Build the host library.
#
#   scripts/build-lib.sh              arm64-v8a libabhost.so (shipping, counters OFF)
#   scripts/build-lib.sh --diag       arm64-v8a libabhost.so with ABR_DIAG_COUNTERS=ON
#   scripts/build-lib.sh --native     x86_64 harness/gltest (local checks only)
#   scripts/build-lib.sh --native --diag
#                                     x86_64 with the diagnostic counters
#
# The CMake caches bake absolute paths, so a moved/renamed project directory
# needs a fresh configure -- this script always re-runs cmake configure, which
# is cheap when nothing changed and correct after a move. Shipping and diag use
# separate build directories so one cache can never leak into the other.
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

NDK="${NDK:-$PROJ/tools/android-sdk/ndk/27.2.12479018}"
JOBS="$(nproc)"
NATIVE=0
DIAG=0
for arg in "$@"; do
    case "$arg" in
        --native) NATIVE=1 ;;
        --diag) DIAG=1 ;;
        *)
            echo "build-lib: unknown argument '$arg'" >&2
            echo "usage: build-lib.sh [--native] [--diag]" >&2
            exit 2
            ;;
    esac
done

if [ "$NATIVE" = 1 ]; then
    BUILD="host/build-native"
    [ "$DIAG" = 1 ] && BUILD="host/build-native-diag"
    # compile_commands.json feeds clangd (host/.clangd points at build-native).
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
    [ "$DIAG" = 1 ] && CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=ON) || CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=OFF)
    cmake -S host -B "$BUILD" "${CMAKE_ARGS[@]}"
    cmake --build "$BUILD" -j"$JOBS"
    echo "native build ok: $BUILD/ (counters $([ "$DIAG" = 1 ] && echo ON || echo OFF))"
    exit 0
fi

if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "build-lib: NDK toolchain not found under $NDK (set NDK=...)" >&2
    exit 1
fi

BUILD="host/build-android"
[ "$DIAG" = 1 ] && BUILD="host/build-android-diag"
CMAKE_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a
    -DANDROID_PLATFORM=android-26
    -DCMAKE_BUILD_TYPE=Release
)
[ "$DIAG" = 1 ] && CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=ON) || CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=OFF)
cmake -S host -B "$BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" --target abhost -j"$JOBS"

LIB="$BUILD/libabhost.so"
echo "android lib ok: $LIB ($(stat -c %s "$LIB") bytes, counters $([ "$DIAG" = 1 ] && echo ON || echo OFF))"
