# Third-party code

ab-port itself is GPL-3.0-or-later (see `LICENSE`). It includes or builds
against the following code under its own licenses.

## In this repository

| File | Origin | License |
|---|---|---|
| `host/src/guest/dlmalloc.c` | Doug Lea's malloc 2.8.6 | MIT-style (notice at the top of the file) |
| `host/src/guest/bsd_random.h` | initial table of the Berkeley `random.c` | BSD-3-Clause, The Regents of the University of California (notice in the file) |
| `games/rio/seeds/cacert.pem` | Mozilla CA certificate bundle (2017, via curl) | MPL-2.0 |
| `patches/dynarmic/*.patch` | changes to dynarmic, below | 0BSD, as dynarmic |

## Fetched into `thirdparty/` (not in git)

| Project | Version | License |
|---|---|---|
| [dynarmic](https://github.com/azahar-emu/dynarmic) | commit `a46601580d5512d324104f985b5f0209dc980ddc` plus `patches/dynarmic/` | 0BSD |
| its externals: mcl, oaknut | as pinned by that commit | MIT |
| its externals: fmt, robin-map, biscuit, zydis, zycore | as pinned by that commit | MIT |
| its externals: xbyak (x86-64 PC builds only) | as pinned by that commit | BSD-3-Clause |
| its externals: Catch2 (tests only) | as pinned by that commit | BSL-1.0 |
| [Boost](https://www.boost.org/) headers | 1.84 | BSL-1.0 |

The Android NDK, SDK build-tools and apktool are build tools, not part of
the output.

## Not included, never distributed

The games themselves (APKs, their native libraries, assets) belong to their
publishers. This repository contains no game files; you supply your own
copy of each original APK. Built APKs are not published here.
