# ab-port

**Run classic 32-bit Android games on 64-bit-only phones, with the games'
own code left untouched.**

New phones (Pixel 7 and later, recent Galaxy, most 2024+ devices) can no
longer run `armeabi-v7a` apps: the CPU and the system only run 64-bit code.
That removed a whole era of games, including the original Angry Birds
series, which their publisher withdrew and never rebuilt. The only
alternative was a paid subscription "virtual phone". ab-port makes the
original APKs run again, natively and offline, as ordinary installed apps.

> **Русский.** ab-port запускает старые 32-битные Android-игры (классические
> Angry Birds и другие) на новых телефонах, которые умеют только 64-битный
> код. Код самих игр не меняется: он выполняется внутри нашей 64-битной
> библиотеки через транслятор dynarmic. Файлов игр здесь нет, нужен свой
> оригинальный APK. Сборка и устройство — ниже, по-английски.

---

## Contents

- [What works](#what-works)
- [How it works](#how-it-works)
- [Building](#building)
- [Installing and playing](#installing-and-playing)
- [Testing and diagnostics](#testing-and-diagnostics)
- [Repository layout](#repository-layout)
- [Adding a game](#adding-a-game)
- [Authorship and credits](#authorship-and-credits)
- [License](#license)

## What works

Each title has a profile (`games/<id>/profile.toml`) that names the exact
original APK it expects (version, signer and SHA-256 of its native
libraries) and a journal (`games/<id>/JOURNAL.md`) with its history and
known issues.

| Title | Version | Engine | Notes |
|---|---|---|---|
| Angry Birds (classic) | 8.0.3 | Rovio fusion | |
| Angry Birds Rio | 2.6.13 | Rovio fusion | |
| Angry Birds Rio | 1.0.0 (2011) | Rovio ka3d, GLES 1.1 | installs beside 2.6.13 as `com.rovio.angrybirdsrio100` |
| Angry Birds Seasons | 6.6.2 | Rovio fusion | |
| Angry Birds Seasons | 3.1.1 | Rovio fusion | |
| Angry Birds Space HD | 2.2.14 | Rovio fusion | |
| Angry Birds Star Wars HD | 1.5.13 | Rovio fusion | |
| Angry Birds Star Wars II | 1.9.25 (Amazon) | Rovio fusion | the Google Play build needs dead servers; use the Amazon one |
| Angry Birds Star Wars II | 2.1 (rework) | Rovio fusion | |
| Angry Birds Stella | 1.1.5 | Rovio fusion | the Zappar scanner says "not available" |
| 99 Bricks Wizard Academy | 2.5.0 | Unity 4.7 + Mono | |
| Doodle Jump SpongeBob SquarePants | 1.02 | Unity 4.3 + Mono | sometimes hangs on the first logo (a race in Unity itself): restart it; short stutters while Mono compiles |
| Minecraft: Story Mode | 1.37 | Telltale | 5.6 GB split set, see its journal |

All of them reach their menus on the test devices, and the Rovio titles have
been played through levels on a phone. Everything runs
offline; network features of the games are off by default (`host.network`
in the profile).

Tested on: a Samsung Galaxy A56 (Android 15, arm64 only) by hand, and a
rooted "Android in Android" sandbox (7.1) that runs an automatic boot gate
for every title.

## How it works

```
 Original APK (unchanged Java + armeabi-v7a engine .so)
        │
        │  scripts/prepare-tree.sh: the engine .so moves to assets/,
        │  Java loads our library instead (one name changed)
        ▼
 ┌──────────────────────────────────────────────────────────────┐
 │ libabhost.so  (arm64, this project)                          │
 │                                                              │
 │  ELF loader ── maps the 32-bit engine into a 4 GB guest      │
 │                address space, applies relocations, gives     │
 │                every import a trampoline                     │
 │                                                              │
 │  dynarmic ──── translates the ARM32 code to arm64 at run     │
 │                time (one shared code cache, fast memory)     │
 │                                                              │
 │  dispatch ──── implements every import the engine calls:     │
 │                libc, pthread, files, sockets, time, assets,  │
 │                OpenGL ES 1.1/2.0, OpenSL ES, NDK sensors,    │
 │                a 32-bit JNI table backed by the real JVM     │
 └──────────────────────────────────────────────────────────────┘
        │
        ▼
 Real Android: ART, the GPU driver, audio, input
```

- **The game's code is never modified.** Its SHA-256 is checked when the
  APK is packaged and again at every start.
- **Calls out of the game** (an `svc` in a trampoline) land in
  `host/src/guest/dispatch.cpp`, which converts 32-bit arguments and
  pointers, does the work with the real Android API and returns. A guest
  pointer that is out of bounds stops the game with a log line containing
  `LOUD`, never a crash of the host.
- **Java stays Java.** The APK's own Java code runs on ART as before; only
  the native half is replaced. Calls from Java into the engine go through
  generated forwarders (`host/gen_apphost.py`), and the engine's JNI calls
  reach the real JVM through a 32-bit JNI function table.
- **Threads are real threads.** Each guest thread is a host thread with its
  own translated CPU state; mutexes, conditions, semaphores and signals keep
  POSIX semantics.
- **Graphics** go straight to the device's GLES driver (`realgl.cpp`), with
  pointer translation and a small state cache.
- **dynarmic** is used with local patches (`patches/dynarmic/`, each one
  explained in its README): a code cache shared by all threads, fast
  indirect-branch dispatch, the TLS register, the generic timer, and more.

`AGENTS.md` is the short technical guide; `REVIEW.md` records findings,
decisions and the regression gate.

## Building

Linux x86-64 is the build host. The Android NDK, build-tools and apktool
are expected under `tools/`, the libraries under `thirdparty/`; both are
gitignored.

### 1. Tools

| What | Version | Where |
|---|---|---|
| Android SDK build-tools | 35.0.0 | `tools/android-sdk/build-tools/35.0.0` |
| Android NDK | r27c (27.2.12479018) | `tools/android-sdk/ndk/27.2.12479018` |
| apktool | 3.0.3 | `tools/apktool_3.0.3.jar` |
| CMake, Make, Python 3, a JDK (17 or newer), `unzip`, `curl` | any recent | system |

With the SDK's `cmdline-tools`:

```sh
sdkmanager --sdk_root=tools/android-sdk "build-tools;35.0.0" "ndk;27.2.12479018" "platform-tools"
```

### 2. Libraries

```sh
git clone https://github.com/azahar-emu/dynarmic thirdparty/dynarmic
git -C thirdparty/dynarmic checkout a46601580d5512d324104f985b5f0209dc980ddc
git -C thirdparty/dynarmic submodule update --init --recursive
for p in patches/dynarmic/*.patch; do git -C thirdparty/dynarmic apply "../../$p"; done

# Boost 1.84 headers: thirdparty/boost/version.hpp must exist
curl -LO https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz
tar xzf boost_1_84_0.tar.gz boost_1_84_0/boost && mv boost_1_84_0/boost thirdparty/boost
```

### 3. A signing key

Android only installs signed APKs. Make one key and keep a backup: an
update must be signed with the same key as the installed app.

```sh
mkdir -p keys
keytool -genkeypair -keystore keys/debug.keystore -alias dbg \
        -storepass android -keypass android -keyalg RSA -keysize 2048 \
        -validity 36500 -dname "CN=ab-port"
```

(`KS`, `KS_ALIAS`, `KS_PASS` override the path, alias and password.)

### 4. The game

Put your own copy of the original APK where its profile says
(`apk.original`, e.g. `games/rio/original/com.rovio.angrybirdsrio.2676.apk`).
Only the exact version in the profile works: its libraries are checked by
SHA-256.

### 5. Build

```sh
scripts/build-lib.sh                 # libabhost.so for phones (arm64, Android 10+)
scripts/prepare-tree.sh rio          # decode the APK, move the engine, apply the port edits
scripts/build-apk.sh rio             # -> games/rio/work/apk_signed.apk
```

`scripts/deploy.sh rio` does the three steps in one and prints the
install commands. After a change to the host only, `scripts/swap-lib.sh
<id>...` replaces the library in already built APKs in about a second.

## Installing and playing

```sh
adb install -r games/rio/work/apk_signed.apk
```

The game installs under its original package name and replaces nothing
else. It starts like any app, offline.

- An installed copy of the original game signed by its publisher must be
  uninstalled first (a different signature); its saves are not carried over.
- The host logs to logcat with the tag `abhost` (`adb logcat -s abhost`), and
  to `/data/data/<package>/files/apphost.log` (readable with root).

## Testing and diagnostics

| Command | What it does |
|---|---|
| `scripts/build-lib.sh --native` | PC build: unit tests, the harness |
| `./host/build-native/fmt_test` | unit tests of the host (expect `ALL OK`); `FMT_BENCH=1` adds timings |
| `./host/build-native/harness <engine.so>` | loads an engine library on the PC and runs its initialisation |
| `scripts/harness-all.sh` | the harness for every title |
| `scripts/devgate.sh <id> [s]` | installs on a test device over ssh, boots, counts error markers, takes a screenshot |
| `scripts/boots.sh <id> <n>` | n boots in a row, for rare failures |
| `scripts/speed.sh` | A/B speed of two builds |

The device scripts read the test device from `scripts/local.env` (copy
`scripts/local.env.example`): a rooted Android reachable over ssh (Termux,
`sudo`). Older Android (7-9) needs the `--sandbox` build of the library
(`scripts/build-lib.sh --sandbox`, `SANDBOX=1 scripts/swap-lib.sh <id>`).

Run-time switches go in `files/abflags.txt` inside the app's data (one
`KEY=VALUE` per line, read at start): `HARNESS_DIAG=1`, `HARNESS_PROF=1`
(a profile of slow frames) and the `HARNESS_NO*` switches listed in
`AGENTS.md`.

## Repository layout

```
host/                 the host library (CMake)
  src/guest/          loader, dispatch, GL/sound/NDK bridges, allocator
  src/apphost.cpp     JNI_OnLoad, boot, threads
  src/fmt_test.cpp    unit tests
  gen_apphost.py      generates the Java->engine forwarders
games/<id>/           one folder per title: profile.toml, JOURNAL.md, smali.patch
patches/dynarmic/     our dynarmic changes, with a README per patch
scripts/              build, package, device gate
docs/                 design notes; docs/history/ is the old investigation log
AGENTS.md             technical guide       REVIEW.md   findings and gate
```

## Adding a game

A new 32-bit game is a new `games/<id>/profile.toml` (copy a similar one)
plus whatever imports it calls that the host does not implement yet: the
first boot logs every missing one with `LOUD`. Unity 4 games and the Rovio
engines are the best trodden paths. Write what you find in its
`JOURNAL.md`.

## Authorship and credits

The code was written by AI models: first through OpenCode with several
models, then by Claude (Anthropic). The project's owner, Shredrum, set the
tasks, made the decisions and tested every build on real devices.

Ideas (no code) came from:
- [MrEmpathy/angry-birds-star-wars-II-arm64](https://github.com/MrEmpathy/angry-birds-star-wars-II-arm64)
- [phys-winner](https://github.com/phys-winner): the Telltale and dynarmic bridges
- [ZailoxTT/ZettaBridge](https://github.com/ZailoxTT/ZettaBridge)

Built on [dynarmic](https://github.com/azahar-emu/dynarmic); see
`THIRD-PARTY.md` for all third-party code.

## License

Copyright (C) 2026 Shredrum

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version. See `LICENSE`.

Angry Birds and the other games are trademarks and copyrighted works of
their owners (Rovio, Sega, Lucasfilm, Nickelodeon, Mojang, Telltale and
others). This project is not affiliated with or endorsed by any of them,
contains none of their files, and does not distribute their games.
