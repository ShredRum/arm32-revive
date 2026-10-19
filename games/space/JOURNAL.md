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

## Root cause found and fixed: missing host `strtod` (2026-10-18)

**Симптом (device, PROVEN):** `Excepted type NUMBER at position 2 but found
STRING` = 783, `Uncaught exception` = 261, `[svc] FATAL abort()` = 261,
`nativeUpdate ended` = 262 → ровно 3 Lua-исключения на кадр, каждый кадр;
меню не появляется (ни точки, ни кнопок).

**Причина (PROVEN, чтением гостя):** `ui/Image.lua` /p5 зовёт
`res.drawSprite("", self.image, 0, 0, "HPIVOT", "VPIVOT", floor(w), floor(h))`
— вызов корректный. Ветку в `drawSprite` (`FUN_002b4038`) выбирает
`lua_isnumber(L,2)` → `FUN_00410ae0` → `luaO_str2d` (`FUN_0041f6fc`):

```c
char *endptr;               /* НЕ инициализирован */
strtod(s, &endptr);
*param_2 = (float)(double)CONCAT44(r1, r0);
if (endptr == s) return 0;  /* читает endptr */
```

Хост `strtod` **не реализовывал**. У гостя каждый UND-импорт получает
трамплин, незнакомое имя проваливается в хвост диспетчера и возвращает
`Ret(regs, 0)` — `endptr` не записан, остаётся стековый мусор →
`luaO_str2d` почти всегда рапортует «число» → строковый аргумент уходит в
ветку NUMBER → исключение в Lua → `abort()`.

Аудит импортов (DIAG-only лог `[import] first call:`): гость реально вызывает
162 разных импорта, `strtod` среди них — это была единственная настоящая
дырка. Остальные 12, не упомянутые в исходниках хоста (`memmem`,
`btowc`/`wctob`/`wctype`, `getauxval`, `setjmp`, `dlerror`, `gethostname`,
`pathconf`, `pthread_condattr_init/destroy`, `pthread_setname_np`,
`__cxa_atexit`), возвращают 0 — ровно как и заглушка; на этом пути не
критичны (в этом прогоне `longjmp` гость вообще не зовёт, поэтому
незаполненный `jmp_buf` от `setjmp` никого не задел).

**Фикс:** ветка `strtod` в `host/src/guest/dispatch.cpp` (гостевой ABI: double
в r0:r1, softfp; `endptr` пишется, `errno` не трогаем).

**Проверка:**
* харнесс: `Excepted type` 3 → **0**, `FATAL abort` 1 → **0**; вотчпоинт на
  самом чекере (`HARNESS_WATCHPC=2fc67c`) больше не срабатывает вообще
  (`watchdump=0`) — падавший путь просто не вызывается;
* Rio: `init done: 54/54 clean, svc=948 halts=53` — без изменений;
* device (установленная либа побайтово наша, md5 сверен): `Excepted type`
  783 → **0**, `Uncaught exception` 261 → **0**, `FATAL abort` 261 → **0**,
  `death.log` = 0 B.

**Что открылось дальше (device):** движок пошёл дальше и дошёл до сети:
6 × `HTTP Error … Unknown SSL protocol error in connection to cloud.rovio.com`,
30 × `[glog] Session login not yet done, retrying later.` — меню ждёт логин
сессии (серверы Rovio в 2026 отдают 301/недоступны). Плюс раски-класс
`[mem] BAD write … -- halting` (3 шт., pc=0x45ad5c, lr=0x43b644),
воспроизводится и в харнессе (9 шт. в watch-прогоне).

## 2026-10-19 — «рандом» при запуске: `pthread_once` съедал чужой `svc #0`

~5 запусков из 10 падали на старте: поток диспетчера уезжал в 0 или в кучу
(`[jit] ExceptionRaised pc=00000000 lr=00546447`, `[mem] BAD exec @ 130ec85c`),
затем `std::future_error` («Promise already satisfied» / «Unknown error»),
`abort()`. Игра это переживает (JIT останавливается, кадр повторяется), поэтому
визуально симптом — пропавшие логотип/сплэш, а не падение.

**Причина.** `halt_va` — общий трамплин `svc #0` («вернуться в хост»), и им
пользуются три несвязанных места: init-рутина `pthread_once`, стартовая функция
созданного потока (её `return` = конец потока) и компаратор qsort. `OnSvc` на
`swi == kSwiHalt` смотрел на **глобальный** `once_depth_ > 0` и заключал «вернулась
init-рутина»: писал `2` в контрольное слово, копировал `saved_regs` инициатора
(PC, SP, r0–r14) в регистры **того потока, который сейчас сделал halt**, и
продолжал выполнение там. Значит выход воркера внутри окна чужой инициализации
съедался как её завершение: контроль помечался «готово», а посторонний поток
телепортировался на стек инициатора и начинал выполнять **его** продолжение.
Дальше — два потока в одном продолжении на одном стеке (испорченные адреса
возврата → `pop {r3,pc}` берёт 0 или мусор кучи), инициализация опубликована
полуготовой, promise закрыт дважды → `abort()`, а мьютекс диспетчера остаётся
захвачен мёртвым потоком (`[mutex] UNLOCK-FOREIGN … owner=<dead>`).

«Рандом» = нужно совпадение выхода постороннего потока с окном инициализации;
окно приходится на старт, где движок как раз пачками создаёт/джойнит воркеров.

**Доказательство на устройстве** (сборка только с инструментикой, md5 либы
`35ff2fa1…`): 5/10 запусков напечатали `STEAL` с `halt_tid != owner_tid`:

```
RUN 2: [once] STEAL: control=1103228c owner_tid=6  halt_tid=9 depth=1 sp=15b58978   (см. ниже: abort там не от подмены)
RUN 4:               control=0073f6b4 owner_tid=8  halt_tid=6                     excraised=2
                     control=0072c460 owner_tid=1001 halt_tid=6 (тот же sp)        excraised=2
RUN 6:               control=130ed014 owner_tid=6  halt_tid=8   badexec=10 futerr=1 abort=1
RUN 9:               control=0073f6b4 owner_tid=9  halt_tid=8   внешне чисто (порча молча)
```

Осторожно с атрибуцией: `abort` в RUN 2 — **не** следствие подмены, он приходит из
`lr=005cf449` (энтропийный код гостя, см. ниже), а `future_error`/`BAD exec` — вот
они от подмены (RUN 6).

Логи: `/tmp/opencode/steal_n{2,4,6,9}.log`. Самый наглядный — `steal_n9.log`
(там порча не вышла наружу, но механизм виден целиком):

```
[once] hijack: control=0073f6b4 routine=005fe709 depth=0 tid=9   init-рутина t9: первым делом pthread_key_create
[once] STEAL:  control=0073f6b4 owner_tid=9 halt_tid=8          возврат стартовой функции t8 съеден как «init завершилась»
[once] routine done, resumed ... tid=8 owner_tid=9              хосту «готово», t8 уехал в сохранённый контекст t9
[call][t9](T2+) pthread_key_create(0073f6d0 005fe731 ...)       настоящий инициатор t9 продолжает рутину
[call][t8](T2+) pthread_setspecific(00000002 15fc7438 ...)      а t8 исполняет продолжение t9
[call][t8](T2+) pthread_rwlock_wrlock(0073f68c ...)
```

Key-singleton помечался инициализированным **до** `pthread_key_create`, и в него
одновременно зашли два потока. В `steal_n6.log` тот же кадр, что и раньше:
после подмены t6 делает lock+broadcast диспетчера, затем `[mem] BAD exec` в куче.

Тот же контроль до и после фикса (`0073f6b4`, routine `005fe709`):

```
до:    hijack tid=9 → STEAL(owner=9, halt=8) → done tid=8 owner=9 → [t9] pthread_key_create
после: hijack tid=9 → [t9] pthread_key_create → done tid=9 owner=9 → [t9] pthread_setspecific
```

До фикса «готово» объявлял посторонний t8 ещё до того, как инициатор сделал
работу; после — работа внутри окна, и «готово» объявляет инициатор.

Косвенный признак в старых логах,
который вёл к тому же выводу: `[once] routine done` приходил **до** lock+broadcast
init-рутины (в чистых прогонах — после), т.е. «завершалась» не она.
Заодно снимается прежняя гипотеза про «обнулённый GOT-слот в RELRO»: `pc=0`
рождался из испорченного контекста/стека, а не из записи в `.got`.

**Фикс** (`79538ef`): отдельный трамплин `kSwiOnce = 0x200000`, который в LR
ставит **только** `PthreadOnce`, поэтому «init-рутина вернулась» однозначно по
построению, а `kSwiHalt` больше не имеет особого случая для once; стек
вложенных перехватов (`once_stack_`/`once_depth_`) стал `thread_local`, т.к.
рутина выполняется на одном потоке. Семантика для гостя не меняется (возврат 0,
слово `1 → 2`), таймаутов/флагов нет. `[once]` теперь печатает `tid/owner_tid`,
а чужой возврат на наш трамплин (возможен только если гость сам прыгнул на
него) печатает `STEAL` и останавливает JIT.

**Что оставалось (не костыль, но настоящая неверность гостю):** при `state == 1`
(`PTHREAD_ONCE_INIT_IN_PROGRESS`) мы всё ещё возвращали 0 не дожидаясь
инициализации — bionic блокирует вызывающего до `2`. Это уже не мелочь: она
доказанно даёт `abort` в 1 запуске из 10 (см. ниже). **Исправлено 2026-10-19 —
раздел «Правка `state == 1`» ниже.**

**Проверка фикса на устройстве** (md5 установленной либы сверен,
`bcecd004…`, 10 запусков): `STEAL = 0`, `BAD exec = 0`, `future_error = 0`,
`ExceptionRaised = 0` во всех 10; `hijack == done` в каждом прогоне, и в каждой
паре `tid == owner_tid`, включая вложенность (`0073f0d8` depth 1 → `0073f0cc`
depth 0). Т.е. телепорт ушёл, а honest-книга событий сошлась.

**Отдельная, но связанная находка — уже не гипотеза.** `[svc] FATAL abort()` с
`lr=005cf449` — это **не** путь телепорта (прежние аборты шли из `lr=000ce2c4`,
ветка `std::terminate` после `future_error`). Дизасм: `0x5cf3e8` — энтропийный
код гостя (`read(fd,buf,len)` в цикле; при `<= 0`, либо при `-1` с
`errno != EINTR`, — `blx abort@plt`). Т.е. гость не смог прочитать энтропию.

Причина названа по факту — сборка, отличавшаяся одним логом
(`[once] LOUD in-progress`, поведение не менялось), прогон 10×:

```
RUN 2 (n2.log):
1580  [once] hijack: control=0073f3e8 routine=005cf24d depth=0 tid=6   ← t6 начал init энтропийного singleton
1584  [once] LOUD in-progress control=0073f3e8 tid=9: returning 0 without waiting (bionic blocks)
1589  [svc] FATAL abort() called (r0=f100010c) pc=f00000cc lr=005cf449
      [thread] tid=9 done retval=f100010c                              ← падает именно t9
```

Поток `t9` спросил `pthread_once` на том же контроле, пока инициатор `t6` был
внутри; получил 0 сразу (наша дырка `state == 1`) и пошёл читать ещё
не закэшированный fd → `read` не удался → `abort()`. Частота: ранний возврат —
2 прогона из 10 (`n1`, `n2`), фатальный исход — 1 из 10 (в `n1` обошлось).

**Правка `state == 1`: блокировать вызывающего (2026-10-19).** Блокируем, пока
контроль не станет `2` — как в bionic. Три части, и все три нужны:

1. само ожидание в `PthreadOnce`: `lock.unlock(); sleep(500 мкс); lock.lock();
   перечитать слово` — тот же идиом, что в `pthread_cond_wait`/`timedwait`, и он
   здесь обязателен: инициатор во время рутины сам ходит в диспетчер
   (`open`/`fcntl`/`read`/`pthread_key_create`), так что ждущий не имеет права
   держать общий замок. Без таймаута. После выхода из ожидания два **разных**
   исхода, и они намеренно разные: `2` → штатный возврат 0; `0` → владелец умер,
   рутина не завершилась → проходим дальше путём свежего вызова и
   инициализируем (иначе тихая ложь меняется на другую тихую ложь, только позже);
2. проверка рекурсии (тот же контроль на **своём** стеке) переставлена **до**
   ожидания — иначе поток ждал бы собственный init, т.е. повис бы навсегда. Это
   не новое поведение, а условие, при котором ожидание безопасно;
3. `ReleaseOnceControlsOnExit(tid)`, вызывается на умирающем потоке в лямбде
   `PthreadCreate` (единственное место, где рождаются гостевые потоки). Если
   владелец остановлен внутри рутины — у нас это возможно (фатальная ошибка
   гостя → halt JIT), в bionic невозможно — контроль сбрасывается в `0`.
   Ожидание, которое нечем отпустить, это ожидание с отказом «навсегда», поэтому
   это не отдельная заплатка, а вторая половина того же изменения.

Семантика для гостя не изменилась (возврат 0, слово `1 → 2`); таймаутов, клампов
и флагов нет. Строки `[once] wait:`/`wait over:` печатаются только под
`HARNESS_DIAG`; громкими без флага остаются две аномалии (`still waiting` ≥1 с и
`stopped inside an init routine`) — это отчёты о проблеме, как `[mem] BAD exec`,
а не отладка.

**Проверка на устройстве** (либа в АПК и установленная — md5
`b81800814db7b0e66db0110919ed1a8a`, `HARNESS_DIAG=1`, 12 запусков по 75 с):

- ожидание **срабатывает** — в 5 прогонах из 12 (`wait_seen=1` в RUN 1, 4, 5, 8,
  11): контроль `0073f3e8` (энтропия) один раз и `00701828` четыре раза. То есть
  тест не пустой;
- во всех пяти ожидающий отпущен исходом `state=2` (инициатор успел завершить):
  RUN 1 `wait: control=0073f3e8 in progress on another thread -- caller tid=8
  blocks here` → `wait over: control=0073f3e8 state=2 -- caller tid=8 resumes` →
  дальше `fopen`/`fcntl` обычным порядком;
- `FATAL abort = 0`, `BAD exec = 0`, `future_error = 0`, `STEAL = 0`,
  `ExceptionRaised = 0` во **всех 12**;
- `still waiting` = 0 (ни одно ожидание не длилось и секунды),
  `stopped inside an init routine` = 0 (страховка не прожата ни разу);
- **прямое «до/после» на одном контроле** — `0073f3e8`, тот же энтропийный
  синглтон и та же форма гонки, что в фатальном `n2.log`:

  | | до правки (`n2.log`) | после (RUN 1) |
  |---|---|---|
  | в init | hijack `tid=6` | hijack `tid=9` |
  | второй поток | `LOUD in-progress control=0073f3e8 tid=9: returning 0 without waiting` | `wait: control=0073f3e8 … caller tid=8 blocks here` |
  | итог | `FATAL abort() … lr=005cf449`, `tid=9 done` | `wait over … state=2` |
  | падений | 1 | 0 |

  Номера потоков в паре меняются от запуска к запуску (планировщик) — важно, что
  форма та же: один поток внутри энтропийного init, второй зовёт once на том же
  контроле. В RUN 1 пользователь играл вручную (лог длиннее прочих: 39949 строк
  против ~37k), но once-события там из старта, до уровня; и вывод не висит на нём —
  остальные четыре прогона с ожиданием (4, 5, 8, 11) никто не трогал.

**Наблюдение побочно:** в RUN 1 удалось вручную зайти на уровень — значит эта
сборка не упирается насмерть в логин/сеть, уровень отрисовывается и принимает
ввод. Разбирать отдельно (п. 0 в Next steps).

**Окно прогона выбрано замером, а не привычкой.** До этой серии стояло 75 с «на
запас». Замер роста лога по секундам (один прогон 75 с, сэмпл каждые 5 с):

| t, с | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 45 | 50 | 55 | 60 | 65 | 70 | 75 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Δ, KiB/5 с | — | 267 | **1482** | 59 | 66 | 78 | 99 | 85 | 84 | 83 | 82 | 89 | 77 | 84 | 82 |

Тяжёлая фаза — загрузка, и она кончается к 15–20 с (всплеск +1482 KiB в
интервале 10–15 с); дальше ровные ~16–18 KiB/с — это уже цикл меню, который ждёт
ввода и ничего нового не проверяет. Все `[once]`-события укладываются в ту же
фазу (строки `[once]` стоят около 1500-й из ~36 500). Поэтому окно сокращено до
**45 с** — двойной запас над смысловой фазой; серии ниже уже на 45 с. Полный лог
замера: `/tmp/opencode/grow.log`.

**Артефакты и серии (что именно проверялось):**

| сборка (md5 либы) | серия | результат |
|---|---|---|
| `b8180081…` | 12 × 75 с, `HARNESS_DIAG` | ожидание 5/12, `state=2` во всех; падений 0 |
| `d76d732d…` (поправлен только текст лога) | 6 × 75 с, `HARNESS_DIAG` | ожидание 2/6, `state=2` во всех; падений 0 |
| `6adc66d2…` — **коммитируемая** (отличается от предыдущей только шириной счётчика ожидания `int` → `int64`, поведение то же) | 1 × 75 с (замер окна) + 4 × 45 с **без флагов** (прод-конфигурация) | 0 падений; `[once] hijack = routine done = 10`; процесс жив |

`stopped inside an init routine` не сработало ни разу ни в одной серии — страховка
осталась непрожатой, как и ожидалось.

**Важная поправка к методу: в проде лог короткий** (~470 строк за 45 с против
~36 500 с диагностикой), потому что основная масса печатей — диагностическая. То
есть по объёму лога в прод-режиме судить о живости нельзя, и «тихий лог» легко
принять за «ничего не работает». Живость проверяется другим: безусловными
строками (`[once] hijack == routine done`, `[apphost]`), кодом движка в его
собственном логе (`[glog] Session login not yet done, retrying later.` — это он
крутит цикл сессии) и процессом (RSS 56 МБ, 70 % ЦП после 86 с, `death.log` = 0 Б).

**Отдельная проверка ЦП/памяти:** утечки нет — VmRSS Rio плоский (74–75 МБ на
протяжении 64 с замера), а каждый прогон это новый процесс, так что накопления
между запусками быть не может.

**Почему это не задело Rio?** Проверено две разные вещи.

(1) В **init-массиве** Rio `pthread_once` нет вообще: нативный прогон Rio даёт
`svc=948 halts=53` до и после правки, т.е. число once-возвратов 0; у Space
`halts` 68 → 66 (два once-возврата, оба в init-массиве).

(2) Но в рантайме Rio его вызвать **умеет** — в его `.so` те же 5 точек
`blx pthread_once@plt`, что и у Space, и в тех же местах:

| | точки вызова `pthread_once@plt` |
|---|---|
| Rio | `0x458430`, `0x45bea6` (игровой код), `0x509636`, `0x509650`, `0x50969a` (обвязка C++ рантайма) |
| Space | `0x53626e`, `0x5383c2` (игровой код), `0x5fe7f6`, `0x5fe810`, `0x5fe85a` (та же обвязка) |

Значит предпосылка (общий трамплин) у Rio та же и путь достижим; в отличие от
init-массива, тут «одним вызовом» это называть нельзя — вопрос в том, доходит ли
дело до `pthread_once` в рантайме и совпадает ли окно с выходом воркера.

**Ответ по устройству (тот же хост, 3 запуска prod-образом, файла флагов нет):**
Rio **доходит** — по 5 `hijack` за запуск, и все чистые: `hijack == done`,
в каждой паре `tid == owner_tid`, `STEAL = 0`, `BAD exec = 0`,
`FATAL abort = 0`; визуально загружается и готов к игре (проверено глазами).
При этом в его init-массиве once нет (см. выше), значит 5 событий — рантаймовые.
Т.е. баг был **достижим** в Rio (дверь та же), но за 3 запуска окно ни разу не
совпало с чужим halt'ом; Space ловит это в 5 запусках из 10 при более широком
окне и плотном верчении потоков на старте. Утверждать, что Rio ловил это раньше,
оснований нет; и наоборот — исключить нельзя, на 3 запусках частота только
ограничивается сверху. Логи: `/tmp/opencode/rio_run{1,2,3}.log`.

## 2026-10-19 — снят кап `cond_wait` + `HARNESS_SPURWAKE` (костыль п.2)

**Что было (по коду).** `pthread_cond_wait` не блокировал: хост подменял его
`pthread_cond_timedwait(now+5 с)` и превращал полученный `ETIMEDOUT (110)` в
успех (`HARNESS_SPURWAKE=1`, ставился в `JNI_OnLoad` безусловно); было и
opt-out `HARNESS_NOCAPWAIT=1`. Родственный кламп жил в `pthread_cond_timedwait`:
гостевой абсолютный дедлайн «дальше 5 с» обрезался до `now+5`. Обоснование капа
в коде — «rendezvous, который никто не сигналит, иначе висит вечно».

**Замер ПЕРЕД правкой (без пересборки, `HARNESS_NOCAPWAIT=1` = ровно целевое
состояние).** Space, diag, 75 с: `nativeUpdate` 633 → 4024, аудио-JNI
(`GetByteArrayElements real len=8192`) 601 → 2365, `spurwake=0`, `abort=0`,
процесс жив (79.8 % CPU, RSS 66 МБ). То есть кап **не несущий** — старая
предпосылка была из эпохи испорченной кучи.

**Правка** (коммит — см. `PLAN.md`): untimed `cond_wait` блокирует как bionic;
кап, `SPURWAKE` и `NOCAPWAIT` удалены; кламп дедлайна в `timedwait` снят (ждём
ровно столько, сколько попросил гость). Печати остались под `HARNESS_DIAG`.

**Проверка на устройстве** (установленная lib `76f2342b4d3b9dc19896b64c05c6dcb1`,
md5 сверен из `base.apk`; до неё на том же устройстве снят baseline на
`b8804206`):

| метрика (окно) | до, кап+SPURWAKE | после, честный wait |
|---|---|---|
| Space прод 75 с | 202 → 212 строк, `[regio]` 60, `[glog]` 51 → 61, abort 0 | 201 → 214 строк, `[regio]` 58 → 60, `[glog]` 51 → 61, abort 0, 72.8 % CPU |
| Space DIAG 45 с | `[cond] wait -> 110` ×6 + `spurwake` ×8 на cond `1080158c` | `[cond] wait` ×2, **все `-> 0`**, `->110` = 0, `spurwake` = 0 |
| кадры/звук на секунду | 49.4 кадра/с, аудио-JNI 30.3/с, mixData 3.79/с | 50.0 кадра/с, 31.3/с, 4.47/с |
| Rio прод 75 с | 168 → 171 строк, `[regio]` 60, abort 0 | 168 → 171, `[regio]` 59 (60-я строка на границе окна), abort 0, 60.4 % CPU |

Вывод: аудиопоток (tid=3, cond `1080158c`), который раньше просыпался **только**
от капа каждые 5 с, без капа работает: смешивание идёт, кадры идут, таймаутов
нет. Rio на общей либе ведёт себя как раньше (хвост `UNLOCK-FOREIGN` +
`unwind` — тот же benign-хвост, что в `rio_run*.log`).

**Соак на зависание** (единственный поздний риск этой правки — редкая потерянная
побудка). Прод-конфигурация, лог и CPU опрашивались раз в 30 с:

| | 30 с | 60 с | 90 с | 120 с | 150 с | 180 с |
|---|---|---|---|---|---|---|
| Space, строк | 203 | 208 | 212 | 212 | 216 | 216 |
| Space, CPU % | 80.9 | 73.8 | 72.7 | 71.1 | 70.6 | 69.9 |

Rio то же за 120 с: 169 → 172 строки, CPU 78.7 → 58.2 %, RSS 11.7 → 49.8 МБ.
`abort=0`, `BAD=0`, `HEAPCORRUPT=0` у обоих; лог растёт (пауза 90→120 с — не
зависание: в проде после загрузки печатает только цикл
`[glog] Session login not yet done, retrying later.`, и он продолжается — та же
картина, что в прод-логах на старом билде: 22/26/30 таких строк за прогон).

**Побочные находки того же класса (в бэклог, не трогали):**
- *sleeps обрезаны до 5 мс*: `nanosleep`/`usleep`/`sleep` (`dispatch.cpp:2605`,
  `:2626`) и остаток в `clock_nanosleep` (`:4385`) — `sleep(60)` вернётся почти
  сразу. Для наших двух образов это пока **дремлющий** путь: Space
  `nanosleep` только импортирует (по `[import] first call` за прогон не вызывал),
  Rio не импортирует вовсе. Честное поведение — спать столько, сколько просят.
- *`pthread_condattr_*` игнорируется*, а `pthread_cond_timedwait_monotonic`
  импортируется Space: такой вызов ушёл бы в cond-переменную, созданную на
  `CLOCK_REALTIME`, то есть «уже истёк» → мгновенный `ETIMEDOUT`. Сейчас это
  латентная дыра фидельности: ни один из двух гостей monotonic-вариант не
  вызывает (проверено по `[import] first call`), а `condattr` без
  `setclock`-эффекта. Починить, когда появится третий образ или жалоба.

## 2026-10-19 — снят `HARNESS_RETSUCC` (костыль п.1): мёртвый код

`HARNESS_RETSUCC=1` ставился прямо в `JNI_OnLoad` («реальный fail-path
стопорится → канем успех», запись 2026-10-14, эра Fox). Проверка по коду:
`SuccInject` вызывается только из PROBE3-хука, а `probe_pc3` бывает ненулевым
**только** под `HARNESS_DIAG` — в поставляемой конфигурации у Space `probe = 0`,
то есть хук не вооружается вовсе. Проверка по логам добавила второе: цель
инъекции — raiser `0x28a3dc` — не транслировался **ни разу** (0
`[probe-emit] … 0028a3dc`, 0 `[succ]`; единственный `probe-emit` в diag —
`00457f90`, это PROBE2). Единственные совпадения `succ=1` в diag-логах — сама
строка `RETSUCC re-armed …`, то есть в каждом прод-логе печаталась ложь.

Правка: `setenv` и эта строка удалены, эксперимент остался opt-in через
`files/abflags.txt` (`HARNESS_RETSUCC=1` + diag). Нативные контракты не
сдвинулись: Rio 54/54 svc=948 halts=53, Space 67/67 svc=1316 halts=66.

Устройство (lib `14c43eda5500250bfdb16aeb43a35180`; md5 сверен из
`base.apk` установленного APK):

| прогон | результат |
|---|---|
| Space прод 75 с | 202 → 212 строк (было 214: −строка RETSUCC, −1 гостевая), `[regio]` 60, `glog` 61, abort 0, 70.5 % CPU |
| Space DIAG 45 с | 31439 строк, `nativeUpdate` 2105, аудио-JNI 1377, **`[succ]=0`, `probe3-emit=0`**, abort 0; на скриншоте — меню с анимацией |

То есть при вооружённых пробах игра доходит до меню **без подставленного
успеха** — «fail-path стопорится» на текущем бейзлайне не воспроизводится.
Следующий в очереди — п.3 (`HARNESS_PROBE=1a410c,fill`).

### Тот же день, проход 3 — `HARNESS_PROBE=1a410c,fill` (п.3): Space не затронут

Этот костыль взводился только для pinned-образа, то есть у Rio; у Space build-id
другой, проба не вооружалась (`[probe]`-строк у него 0 и до, и после). Space в
этом проходе — **контроль**: та же установка, 75 с прода → 212 строк, как до
правки, abort 0, меню на скриншоте. Сам костыль у Rio писал `"{}"` только в
пустой SSO-слот, а слот к тому моменту уже заполнен — его собственная строка
`[probe] OUT non-empty, kept`; так что ушло 16 строк лога и ничего больше.
Детали и «до/после» — `PLAN.md`, проход 3, и `../rio/JOURNAL.md`.

### Тот же день, проход 4 — скаффолдинг tapbeat (п.6): у Space и так не срабатывал

JNI-резолв `MyInputHandler` теперь выполняется только под `HARNESS_TAPBEAT`.
У Space строки `[tapbeat] handler mock=` в прод-логах не было и раньше (резолв
не доходил до успеха), поэтому его прод-лог — **контроль**: 212 строк до и после,
abort 0, меню на скриншоте. У Rio ушла ровно одна строка (154 → 153). Детали —
`PLAN.md`, проход 4, и `../rio/JOURNAL.md`.

### Тот же день, проход 5 — честные sleeps (п.8), последний костыль

Кламп 5 мс снят в `nanosleep`, `usleep`/`sleep` и в alarm-ioctl
`ANDROID_ALARM_SET_AND_WAIT` (в первой записи таблицы он был ошибочно назван
`clock_nanosleep`). Для Space это дремлющий путь: `nanosleep` он импортирует, но
за прогон не вызывал, а alarm-ожидание не сработало (иначе прогон бы застрял).
Проверка на устройстве (lib `9dfa1087…`): 213 строк (`[regio]` 60, `glog` 61 —
как до правки; +1 — benign `[mutex] UNLOCK-FOREIGN`, он гуляет 4–5 за прогон),
abort 0, 70.3 % CPU, меню на скриншоте, застоя строк нет.
Детали — `PLAN.md`, проход 5, и `../rio/JOURNAL.md`.

## Next steps

0. **Утечка при повторных входах в уровень — теоретический вопрос (2026-10-19).**
   В Rio был именно такой баг, но он был **наш**: `pthread_create` брал 8 МБ под
   стек гостя из эмулируемой кучи и не возвращал (≈123 потока → 985 МБ → OOM →
   bad free → SIGABRT; §138/F1 в `AGENTS.md`). Фикс общий (`ReapThreadStack` в
   общей `libabhost.so`), поэтому Space от этого механизма защищён, и он его уже
   ловил (его `VideoRewards`/time-sync цикл). Остаточные кандидаты, если когда-то
   понадобится: (а) собственные аллокации игры при повторном входе — наблюдать,
   не «лечить» костылём; (б) наша бухгалтерия: `ThreadState` живёт в `threads_`
   до join, у detached-потоков не стирается (~сотни байт на поток; чистится
   erase'ом при detach+done). Дешёвая проверка: 10 входов/выходов или долгий
   сеанс в уровне + RSS и `thr=`/`[heap]` в diag; утечка = линейный рост RSS на
   заход, а не плато.

-1. **`pthread_once` (`state == 1`)** — **сделано 2026-10-19**: блокировка
   вызывающего до завершения инициализации (как bionic) плюс отпускание контроля,
   если поток-владелец умер внутри рутины. Проверено на устройстве; числа и
   «до/после» — в разделе выше. Безусловные `[once]`-печати вынесены под
   `HARNESS_DIAG` (см. п. 6).
0. **Логин/сеть** — **решено не трогать** (2026-10-19). Обоснование: Angry Birds
   играется офлайн, сеть для игры не нужна; «локальный ответ сессии» был бы ровно
   тем firewall'ом, которого мы не хотим, а честно неработающая сеть ведёт себя
   как телефон в самолёте — игра это переносит (уровень входил вручную). Остаётся
   только разобрать `[mem] BAD write` @ pc=0x45ad5c (FUN_0046ab40, lr=0x43b644) —
   это уже не про сеть.
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
6. **Диагностики — под один флаг (аудит костылей).**
   - ✅ **Сделано (2026-10-19):** безусловные **нормальные** `[once]`-строки
     (`hijack`, `routine done`) уведены под `HARNESS_DIAG`; аномалии (`STEAL`,
     `still waiting`, `stopped inside an init routine`, `recursive once`,
     `depth exceeded`) и ошибки остаются громкими без флага. Проверка: прод-лог
     Rio короче ровно на число once-строк (364 → 354 при `hijack=done=5`), в
     diag-режиме строки на месте.
   - ✅ **Сделано (2026-10-19):** и остальные нормальные дампы уведены под флаг
     (лог-онли: `DiagOn()` в существующих «первые N»-охранниках). Замер на
     устройстве: Space 446 → **210** строк, Rio 354 → **168**; с `HARNESS_DIAG=1`
     Space снова даёт **31684** строки. Ушли `[fcntl]` 79/58, `[files] fopen->NULL`
     ~25/32, `[realloc]` 30/30, `[files] pipe()` 11/9, `[cond]` 22/4 (включая дамп
     сырых 128 Б памяти гостя), `[strdup]` 20/20, `[netsock]` 9/5, `[jni] region`
     8/8, `[poll]` 5/4, `[alarm]` 3/3, `[realgl]` 25/13 (остался один `live ctx`;
     `BAD-PTR` виден всегда). Таблица «было → стало» — `PLAN.md`.
   - Поведенческие костыли (не печати!) — отдельная таблица там же:
     `RETSUCC`, ~~кап `cond_wait` + `SPURWAKE`~~ (**снято 2026-10-19**, см. раздел
     выше), `HARNESS_PROBE=1a410c,fill` (у Rio build-id совпадает с пришитым,
     поэтому PC-проба вооружена именно у него — его 15 строк `[probe]` уйдут
     вместе с этим пунктом) и **скаффолдинг
     tapbeat** (резолв `MyInputHandler` через JNI на первом `nativeUpdate`
     безусловно, хотя инъекция входа выключена).

### Shared lib: background audio forwarding (2026-10-19)

The `AudioOutput.stopOutput()`/`requestExclusiveAudio()` forwarding added for
Rio (a299190, AGENTS §143) lives in the shared host lib and applies to Space
too. Space prod: `apk_signed.apk` md5
`93fd811854a02ec45498116afdde5531`, lib
`7a7e731d194c08bd6ca29bbb4d4ee0fd`, versionCode 221400; native init 67/67
clean. No Space-specific background-audio symptom was reported and no Space
device run was made; this is the same shared call path.
