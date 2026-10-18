# Angry Birds Space HD — port journal

**Status: PORT STARTED — loader stage PASSES** (2026-10-18). The untouched ARM32
guest loads, relocates, runs its whole init array and returns JNI_VERSION_1_6
from `JNI_OnLoad` on our host. Nothing is shipped yet; there is no device build.

Identity / machine-readable facts: `profile.toml` (same directory).

## Spike result (the go/no-go measurement)

```
./host/build-native/harness games/space/work/lib/armeabi-v7a/libAngryBirdsSpace.so
→ loaded ...: 314 imports (5 pending data), 67 init, 1 fini, halt=f0000000
→ init done: 67/67 clean, svc=1296 halts=68
```

```
HARNESS_ONLOAD=cec08 HARNESS_JNI=cec08 ./host/build-native/harness <space.so>
→ [jni] JNI_OnLoad @000cec08 jvm=f1002004 ... hr=16777216 r0=00010006
```

`r0 = 0x00010006` = JNI_VERSION_1_6 — a correct return. The Rio baseline is
unchanged by everything below (`init done: 54/54 clean, svc=948 halts=53`).

**Verdict: same engine family, port is feasible.** Imports are the same *surface*
as Rio (`__stack_chk_guard`, `_tolower_tab_`, `__sF`, `_toupper_tab_`, `_ctype_`
are the pending data imports in both), the same 2-PT_LOAD layout convention, and
the init array runs on the generic loader.

## Core bugs this guest exposed (FIXED, commit `a677dea`)

Both are *generic* defects in the guest core, not Space specials; Rio simply
never tripped them.

1. **`pthread_once` hijack was single-slot.** An init routine may legitimately
   call `pthread_once` for a *different* control variable; the one-slot guard
   called that "recursive once" and returned "done" without running the nested
   routine, leaving its guard uninitialised. Space does exactly this in its init
   array (control `0x73f0d8` nested inside `0x73f0cc`). Now a LIFO stack
   (depth 16); only the *same* control reappearing while in progress is
   pathological recursion.
2. **Resume PC kept the Thumb interworking bit.** `Ret()` strips bit 0
   (`regs[15] = lr & ~1`) and moves it to CPSR; the once-resume assigned the raw
   LR, so PC was odd (e.g. `00536273`), the JIT fetched from an odd address and
   mis-decoded everything after — Space died with a wild read at `d40073f0`.

Effect on Space: init **9/67 → 67/67 clean**.

## Guest facts (measured, not inferred)

| item | Space 2.2.14 | Rio 2.6.13 (for comparison) |
|---|---|---|
| size / sha256 | 7 341 052 B / `cc11a1e9…` | 6 759 156 B / `980fad19…` |
| PT_LOAD | `0..0x6e812c` r-x, `0x6e9e78..+0x17048` rw (`memsz 0x56e38`) | analogous |
| `.text` | `0x31000 + 0x5e691c` | — |
| `.ARM.exidx` | VA `0x654790`, 14006 entries | — |
| `.bss` | VA `0x700ec0`, size `0x3fdf0` | — |
| undefined imports | 314 (5 pending data) | 338 (same 5) |
| `DT_INIT_ARRAY` | 67 entries, 1 fini | 54 entries |
| GNU build-id | **absent** (only `.note.gnu.gold-version`) | present, 20 B |
| `JNI_OnLoad` | `0xcec08` → returns `0x10006` | `0xc2db4` |
| `Java_*` exports | 62 | 63 |
| JNI namespace | `com.rovio.fusion` | `com.rovio.fusion` |

Because Space carries **no** GNU build-id note, `CheckGuestBuildId()` LOUDs
(`loaded (0 B)` vs pinned) and continues. That is the intended degradation; the
build-id is not a usable discriminator *for Space*, but its **absence** is a
clean "not Rio" marker.

## First functional walls (next work)

* `nativeConfig` (`0xd0454`) calls `GetStringUTFChars(NULL)` → the harness passes
  r2/r3 = 0; it wants a jstring argument (Rio's flow used `HARNESS_JSTR`).
* `nativeInit` (`0xd3864`) runs **real engine code**, reaches a string compare
  (`"min"` vs a host-provided string) and then a wild read:
  `BAD read @ 01ae6328 size 4 guest-pc=00557538 r4=0028fe73` — `0x1ae6328` is
  outside the image, so an engine pointer got a bogus value. This is where the
  port campaign begins.

## Tools that made this possible (reuse these)

* **Thumb-2 disassembly** (distv is Thumb16-only — 32-bit encodings print as two
  `UNKNOWN` halves, do not trust the listing):
  ```
  tools/android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump \
    -d --triple=thumbv7-none-eabi --start-address=0x536234 --stop-address=0x5362b0 <guest.so>
  ```
  (`--triple=thumbv7` is what makes it decode Thumb without `$t` mapping symbols.)
* **Memory watchpoints** (already in the harness, only in `MemoryWrite32` — bulk
  stores bypass): `HARNESS_WPA=<hex VA>` = who writes here (prints pc + regs);
  `HARNESS_WP=<hex value>` = who materialises this value.
* **JNI stage**: `HARNESS_ONLOAD=<addr>` (default is Rio's `0xc2db4` — always set
  it for a non-Rio image) and `HARNESS_JNI=<addr>[,<addr>…]`.

## Progress log

* 2026-10-18: `scripts/prepare-tree.sh space` OK — decode clean, guest moved to
  `assets/guestlib/armeabi-v7a/` STORED, `Gen.smali` patched (`AngryBirdsSpace`
  → `abhost`, 1 occurrence, so the profile's EXPECTED lib name was right),
  sha256 re-verified. No `seeds/` yet (a fresh game captures them on first run).
* 2026-10-18: `work/jni_signatures.txt` extracted from the smali — exactly the
  62 signatures of the guest's 62 `Java_*` exports (79 native methods are
  declared; the other 17 belong to other libs in the APK: Camera, Location,
  Share, VideoPlayer). Compared with Rio: **60 shared names, all with identical
  signatures, 0 conflicts**; 3 Rio-only (`fusion/WebViewWrapper`), 2 Space-only
  (`fusion/RemoteNotificationsClientWrapper`).
* 2026-10-18: `gen_apphost.py --union` (commit `2880539`) — one `libabhost.so`
  for all titles; union is now 65 forwarders (63 rio + the 2 space-only),
  purely additive for Rio, and the per-game path reproduces the committed `.inc`
  byte-for-byte.

## Game-specific surface actually in **prod** code (verified inventory)

A hex-literal scan of the prod sources (comments stripped) corrects the earlier
"53 engine VAs" figure — that counted comments and dev pokes. The **real**
per-game constants are ~10 package/lib sites and ~20 VA sites:

* package / files dir / log tag: `apphost.cpp` 92, 143, 1010, 1011, 1021, 1313;
  `dispatch.h` 510; `dispatch.cpp` 2776, 7411; (`harness.cpp` 751, dev only)
* guest lib asset name: `apphost.cpp:1313`
* pinned build-id: `elf_loader.h` (`PinnedGuestBuildId()`)
* probe slot `1a410c`: `apphost.cpp:1177` + `dispatch.cpp:8345`
* engine VAs in prod: `apphost.cpp` `65E650`, `5FE70`, `C23E4`, `5885C`;
  `dispatch.cpp` `45A778`, `457FE7`, `D5994`, `59D250`, `28AE70`,
  `28A380/28B0E4/28B160/28B1B8/28B390/28B3E8/28B484`, `672E98`
* layout **bounds** (Rio-layout-specific, must be re-checked for Space):
  `653D1C`, `6EE000`, `650000`, `600000`, `6F0000`
* everything else (`2B0C50`, `C2144`, `250C80`, `263300`, `283B5C`, …) is
  behind a `HARNESS_*` poke flag → inert in prod.

## Прогресс: runtime-identity + ABI-фикс exidx + устройство

### Runtime-identity (шаг 1 плана — сделан)

`host/src/appid.h`: `AppPackage()` выводит пакет из `/proc/self/cmdline` (в
Android имя главного процесса И ЕСТЬ пакет; `:proc` срезается; если это путь —
fallback), `GuestFilesPrefix()`. Использование:

* `apphost.cpp`: каталог files, поиск APK по префиксу пакета в `/data/app`,
  **гостевая либа и seeds находятся листингом zip** (`ZipWalk` + `ZipListPrefix`;
  добавлен листинг центрального каталога, извлечение STORED осталось тем же);
* `dispatch.h`: дефолт `files_prefix_`; `dispatch.cpp`: мок
  `getPathToFileCacheDirectory`;
* лог-тег Android теперь `abhost` для всех тайтлов (в OnLoad-строке печатается
  пакет). Rio-APK при этом строится из тех же констант бит-в-бит
  (`com.rovio.angrybirdsrio`).

### Мостик файлов — правило стало общим

Любой гость-путь вида `/data/{data,user/<N>}/<pkg>/files<rest>` отображается в
host-files. Харнесс задаёт префикс Rio (`SetFilesDir(...)`) и раньше путь Space
не мапился → ложная стена `unmapped guest path`. Мы хостим одного гостя, так что
компонент пакета знать не обязательно (на устройстве гость и так видит только
свой каталог).

### `__gnu_Unwind_Find_exidx`: починен ABI (для не-pinned образов)

Настоящий ABI: `(pc, int* count)` → **база таблицы** exidx, в `*count` — число
8-байтных записей; сам гость (его статический libgcc) дальше делает бинарный
поиск. Наш стаб возвращал **отдельную запись** для `pc` и не писал `count`.

* ПРОВЕРЕНО: Rio этот импорт в прогоне харнесса **не вызывает** (0 срабатываний),
  но на fail-path (prod) вызывает — поэтому pinned-образ сохранён бит-в-бит;
* Space: получил нашу запись как «базу», искал ~2.7М записей с мусорным count →
  wild read `0x1AE6328` при `guest-pc=0x557538` (`ldr r3,[r0]`, ARM-код, не Thumb:
  регион — ARM; `HARNESS_EXIDXLOG` показал `pc=0051fce5 -> entry=00666f90`, и
  00666f90 — ровно та «база», по которой гость улетел);
* после фикса дикого чтения нет.

Важно: `.ARM.exidx` на **prod-пути уже выводится** из модуля
(`Runtime::FindExidx` → `mod_.exidx_va/exidx_count`); хардкод `59D250/18184`
остался только в dev-диагностике/TTUNW → для буста не критичен.

### Устройство (первая установка Space)

`sudo pm install -r ~/ab_space.apk` (в Termux `pm` — это
`PATH=/system/bin exec /system/bin/pm`, ставить надо под `sudo`/tsu: иначе
`SecurityException: runInstallCreate ... user -1`). Пакет
`com.rovio.angrybirdsspaceHD`, Rio не задет.

Хост отработал end-to-end: `init done: 67/67`, `guest OnLoad r0=00010006`,
`product live`; файлы созданы в `/data/user/0/com.rovio.angrybirdsspaceHD/files`
(значит runtime-identity верна и на устройстве), `death.log` 0 Б, гость
извлечён из APK (7341052 Б). Блокер был тот же wild read.

### Харнесс после фиксов (порядок как в приложении: config → init)

```
HARNESS_ONLOAD=cec08 HARNESS_JNI=d0454,d3864 \
HARNESS_JSTR=/data/data/com.rovio.angrybirdsspaceHD/files ./host/build-native/harness <space.so>
```

* `nativeConfig` возвращает объект (`r0=10003ad8`), НЕ падает;
* `nativeInit` **завершается чистым halt** (`hr=UserDefined1`, `svc=360448`,
  13 гостевых потоков, GL-текстуры, Lua-строки `new/res/lua/nil`);
* `data/script_paths.json` нет НИ в одном оригинале — движок переживает
  отсутствие, если `nativeConfig` был вызван первым (без него — `IOException:
  Cannot find script commands.lua in script paths` → `abort()`).

Baseline Rio не изменился: `init done: 54/54 clean, svc=948 halts=53`.

## Next steps

1. **Space `assets/seeds/`**: движку нужен `fusion.registry` в files-каталоге
   (после общего правила мостика видно, что он его открывает). Взять тот же
   first-launch набор, что у Rio-порта (он лежит в `games/rio/work/.../assets/seeds/`);
   проверить, какие из них реально нужны Space.
2. Дальше по фронту: `nativeUpdate` (`0xd5e74`) / `nativeRender` (`0xcebec`)
   несколько раз подряд; затем `nativeResize` (`0xcebb4`) + `nativeInput`.
3. Пересобрать android-либу + Space APK, переустановить, читать логи. Затем
   пересобрать Rio APK и проверить, что устанавливаемый Rio-образ не изменился
   (identity/мостик), т.к. изменения затрагивают общий код.
4. Вывести layout-границы из модуля (сейчас хардкод в dev-диагностике; на
   prod-пути exidx уже выведен) — когда будем трогать разворачиватель.
5. `PORTING.md` from the path actually walked.
