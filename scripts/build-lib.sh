#!/bin/bash
# Build the host library.
#
#   scripts/build-lib.sh              arm64-v8a libabhost.so (for the APK)
#   scripts/build-lib.sh --native     x86_64 harness/gltest (local checks only)
#
# The CMake caches bake absolute paths, so a moved/renamed project directory
# needs a fresh configure -- this script always re-runs cmake configure, which
# is cheap when nothing changed and correct after a move.
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

NDK="${NDK:-$PROJ/tools/android-sdk/ndk/27.2.12479018}"
JOBS="$(nproc)"

if [ "${1:-}" = "--native" ]; then
    cmake -S host -B host/build-native -DCMAKE_BUILD_TYPE=Release
    cmake --build host/build-native -j"$JOBS"
    echo "native build ok: host/build-native/"
    exit 0
fi

if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "build-lib: NDK toolchain not found under $NDK (set NDK=...)" >&2
    exit 1
fi

cmake -S host -B host/build-android \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build host/build-android --target abhost -j"$JOBS"

LIB=host/build-android/libabhost.so
echo "android lib ok: $LIB ($(stat -c %s "$LIB") bytes)"
