cmake_minimum_required(VERSION 3.22)
project(AbrHost C CXX)

# Stage D: leak counters are a compile-time diagnostic build only. The default
# shipping build has no counter fields, increments, symbols or dump strings.
option(ABR_DIAG_COUNTERS "Build the host with diagnostic leak counters" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dynarmic: A32 frontend only, no tests — keeps the build lean.
# Boost: portable headers in thirdparty/boost (verified tarball).
# BOOST_ROOT must contain boost/version.hpp, i.e. thirdparty/ itself.
set(BOOST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty CACHE PATH "" FORCE)
# Cross-compiling (Android NDK) confuses FindBoost's search; hand it the dir.
set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/dynarmic dynarmic)

# Native smoke test: runs hand-assembled ARM code through the JIT.
add_executable(smoke_a32 src/smoke_a32.cpp)
target_link_libraries(smoke_a32 PRIVATE dynarmic)

# Micro-test for isolated instruction checks.
add_executable(dispatch_bench src/dispatch_bench.cpp)
target_link_libraries(dispatch_bench PRIVATE dynarmic)

add_executable(cbnz_test src/cbnz_test.cpp)
target_link_libraries(cbnz_test PRIVATE dynarmic)

# Unit test for the guest printf formatter (variadic ABI of the guests).
add_executable(fmt_test src/fmt_test.cpp)
target_link_libraries(fmt_test PRIVATE guest)

# Mini-zip reader for the app's own base.apk (the guest .so and the seeds).
# Its own module so the native unit test can reach the extraction.
add_library(zipfile STATIC src/zipfile.cpp)
target_include_directories(zipfile PUBLIC src)

# Unit test for the zip extraction (the seed path: a short read must not
# leave a truncated file behind).
add_executable(zip_test src/zip_test.cpp)
target_link_libraries(zip_test PRIVATE zipfile)

# distv: Dynarmic-based disassembler for the game .so.
add_executable(distv src/distv.cpp)
target_link_libraries(distv PRIVATE dynarmic)

# Guest runtime library (ELF loader + SVC dispatch + heap).
add_library(guest STATIC
    src/guest/elf_loader.cpp
    src/guest/dispatch.cpp
    src/guest/realgl.cpp
    src/guest/apk_vfs.cpp
    src/guest/opensl.cpp
    src/guest/dlmalloc.c
)
# dlmalloc as a bounded mspace (no sbrk/mmap fallback, 8B alignment like
# Bionic-era ARM32, external locking via our bridge mutex).
# FOOTERS=0 for shipping: FOOTERS is a debug layout that (a) stores a host
# mspace pointer in guest-visible memory at every chunk foot and (b) makes
# mspace_free dereference that foot first (a smeared foot => wild deref).
# DEBUG is deliberately LEFT UNDEFINED (2026-10-17, PLAN.md C2): dlmalloc
# selects its assert variant with `#ifdef DEBUG` (dlmalloc.c:1452), so passing
# `-DDEBUG=0` DEFINED it and compiled in the abort-on-assert build (real crash
# risk in shipping). Undefined, the `#else` arm gives `#define assert(x)` and
# then `#define DEBUG 0`, so every later `#if DEBUG` still evaluates to 0.
# Real corruption still aborts through dlmalloc's erroraction (RTCHECK/ok_*,
# C3), so the firewall stays meaningful. Both FOOTERS/DEBUG were hunt tools
# (2026-10-16).
set_source_files_properties(src/guest/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;MALLOC_ALIGNMENT=8;USE_LOCKS=0;NO_MALLINFO=1;NO_MALLOC_STATS=1;FOOTERS=0")
# Corruption hook header (replaces dlmalloc's abort() with an atomic tripwire).
set_source_files_properties(src/guest/dlmalloc.c PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/guest/abr_heap_hook.h")
target_include_directories(guest PUBLIC src)
if(ABR_DIAG_COUNTERS)
    # PUBLIC: abhost.cpp and the native harness/fmt_test also include
    # dispatch.h and must see the guarded declaration.
    target_compile_definitions(guest PUBLIC ABR_DIAG_COUNTERS=1)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        target_compile_definitions(dynarmic PUBLIC DYNARMIC_DISPATCH_COUNTERS=1)
    endif()
    message(STATUS "ABR diagnostic counters: ON")
endif()
target_link_libraries(guest PUBLIC dynarmic z)
if(NOT ANDROID)
    target_link_libraries(guest PUBLIC EGL GLESv2 X11)
else()
    target_link_libraries(guest PUBLIC EGL GLESv2 android OpenSLES)
endif()
# Private frontend header (a32_ir_emitter.h) for the HARNESS_PROBE IR emitter.
target_include_directories(guest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/dynarmic/externals/mcl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/dynarmic/externals/fmt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty)

# Bring-up harness: loads the real game .so, runs DT_INIT_ARRAY.
add_executable(harness src/harness.cpp)
target_link_libraries(harness PRIVATE guest)
# Always keep debug info (forensics on cores; same -O codegen, no timing change).
target_compile_options(harness PRIVATE -g)
target_compile_options(guest PRIVATE -g)

# Bring-up probe for new titles: load any guest .so, run its inits, call exports.
add_executable(guestrun src/guestrun.cpp)
target_link_libraries(guestrun PRIVATE guest)
target_compile_options(guestrun PRIVATE -g)

# Product host (Android only): real Java drives guest Java_* under Dynarmic.
if(ANDROID)
    add_library(abhost SHARED src/apphost.cpp)
    target_link_libraries(abhost PRIVATE guest log zipfile)
    # Android 15+ devices may run with 16 KB pages: LOAD segments must be
    # aligned to 16 KB or the library does not load there. Harmless on 4 KB
    # devices (it only adds alignment padding).
    target_link_options(abhost PRIVATE "LINKER:-z,max-page-size=16384")
endif()

# Harness-only real-GLES compile check (PC GPU validation of game shaders).
if(NOT ANDROID)
    add_executable(gltest src/gltest.cpp)
    target_link_libraries(gltest PRIVATE EGL GLESv2 X11)
    add_executable(eglmin src/eglmin.cpp)
    target_link_libraries(eglmin PRIVATE EGL GLESv2)
endif()
