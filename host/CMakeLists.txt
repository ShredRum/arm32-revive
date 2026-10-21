cmake_minimum_required(VERSION 3.22)
project(AbrHost C CXX)

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
add_executable(cbnz_test src/cbnz_test.cpp)
target_link_libraries(cbnz_test PRIVATE dynarmic)

# Unit test for the guest printf formatter (variadic ABI of the guests).
add_executable(fmt_test src/fmt_test.cpp)
target_link_libraries(fmt_test PRIVATE guest)

# distv: Dynarmic-based disassembler for the game .so.
add_executable(distv src/distv.cpp)
target_link_libraries(distv PRIVATE dynarmic)

# Guest runtime library (ELF loader + SVC dispatch + heap).
add_library(guest STATIC
    src/guest/elf_loader.cpp
    src/guest/dispatch.cpp
    src/guest/realgl.cpp
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
target_link_libraries(guest PUBLIC dynarmic z)
if(NOT ANDROID)
    target_link_libraries(guest PUBLIC EGL GLESv2 X11)
else()
    target_link_libraries(guest PUBLIC EGL GLESv2)
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

# Product host (Android only): real Java drives guest Java_* under Dynarmic.
if(ANDROID)
    add_library(abhost SHARED src/apphost.cpp)
    target_link_libraries(abhost PRIVATE guest log)
endif()

# Harness-only real-GLES compile check (PC GPU validation of game shaders).
if(NOT ANDROID)
    add_executable(gltest src/gltest.cpp)
    target_link_libraries(gltest PRIVATE EGL GLESv2 X11)
    add_executable(eglmin src/eglmin.cpp)
    target_link_libraries(eglmin PRIVATE EGL GLESv2)
endif()
