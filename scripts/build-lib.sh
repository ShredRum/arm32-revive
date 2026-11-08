#!/bin/bash
# Build the host library.
#
#   scripts/build-lib.sh              arm64-v8a libabhost.so for phones (shipping, counters OFF)
#   scripts/build-lib.sh --diag       arm64-v8a libabhost.so with ABR_DIAG_COUNTERS=ON
#   scripts/build-lib.sh --sandbox [--diag]
#                                     the same for the sandbox (Android 7.1)
#
# Phone builds target API 29 (Android 10): from there thread_local is ELF TLS,
# one instruction, where API < 29 calls __emutls_get_address and
# pthread_getspecific on every access (measured 2026-11-08 on a phone: ~8 %
# of Rio's game thread). Every 64-bit-only phone runs Android 12 or newer;
# on Android 7-9 the dynamic linker refuses a library with a TLS segment,
# hence the API 26 --sandbox build (host/build-android-sandbox[-diag]); the
# two differ only in how thread_local is reached. swap-lib.sh, build-apk.sh
# take it with SANDBOX=1; devgate.sh refuses a phone build on the sandbox.
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
SANDBOX=0
for arg in "$@"; do
    case "$arg" in
        --native) NATIVE=1 ;;
        --diag) DIAG=1 ;;
        --sandbox) SANDBOX=1 ;;
        *)
            echo "build-lib: unknown argument '$arg'" >&2
            echo "usage: build-lib.sh [--native | --sandbox] [--diag]" >&2
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
API=29
if [ "$SANDBOX" = 1 ]; then
    BUILD="$BUILD-sandbox"
    API=26
fi
CMAKE_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a
    -DANDROID_PLATFORM=android-$API
    -DCMAKE_BUILD_TYPE=Release
)
[ "$DIAG" = 1 ] && CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=ON) || CMAKE_ARGS+=(-DABR_DIAG_COUNTERS=OFF)
cmake -S host -B "$BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" --target abhost -j"$JOBS"

LIB="$BUILD/libabhost.so"
echo "android lib ok: $LIB ($(stat -c %s "$LIB") bytes, counters $([ "$DIAG" = 1 ] && echo ON || echo OFF), API $API$([ "$SANDBOX" = 1 ] && echo ", sandbox" || echo ", phones"))"
