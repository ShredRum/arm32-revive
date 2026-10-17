# PLAN — перф и чистка (порт Angry Birds Rio → arm64)

Создан 2026-10-17. Назначение: единое место для бэклога оптимизаций/чистки, чтобы он
не терялся при компрессии контекста (AGENTS.md — это журнал, он замусорен).
Сопутствующие документы: `AGENTS.md` (история/журнал), `work/ARCH.md` (архитектура, частично устарел).

Состояние на момент создания: рабочее дерево закоммичено, доступно по тегу
**`works-2026-10-17-dlmalloc-stable`** («first stable dlmalloc build»).
Что доказано на устройстве: соак ~4.8 мин, pid не менялся, `death.log` пуст, ни
`LOUD`/`HEAPCORRUPT`, новых tombstone нет, меню рисуется, дельты `nativeUpdate` 16/18 мс
(≈55–60 fps). Лог ~11.3 МБ за прогон. md5 установленной либы: `fd798e09b3076ab25901ffff788a57c5`.

## Решения пользователя (2026-10-17)
1. Вся диагностика — за **одним** `HARNESS_DIAG`, по умолчанию **OFF** в проде; код
   остаётся дремлющим и включается на прогон через `files/abflags.txt`.
2. `ClampHeapWrite`/`heap_sizes_`: **пробуем OFF по умолчанию**, потом смотрим.
3. `-DDEBUG=0` в dlmalloc (сейчас включает debug-assert вместо выключения) — **починить**.
4. `work/` (4.1 ГБ) — **не чистить** сейчас, решится позже.
5. Новый git tag — только после всего плана и по отдельной команде. Коммиты по ходу — можно.
6. Порядок: сначала батч **A** (лог-мусор), потом **C1** (JIT) — принято.
7. `RETSUCC`/`SPURWAKE` в этот проход **не трогаем** (это поведение, не диагностика).
   Вынести отдельным экспериментом с гипотезой: «диагнозы от 2026-10-14 (real fail path
   stalls on Fox; cond_wait 110 / audio loader died) делались во времена порчи кучи и
   могли быть её симптомами — перепроверить на стабильном бейзлайне».
8. JIT — двухшагово: (а) замер A/B через `abflags.txt` (нулевой риск в коде);
   (б) если чисто — флипнуть дефолт в коде на `all_safe_optimizations` **вместе** с явным
   выключателем `HARNESS_NOOPT=1` и печатью режима в строке `flags:`. Все 4 площадки
   (`dispatch.cpp:5565/5687`, `apphost.cpp:507`, `harness.cpp:612`) держать согласованными.
9. Язык PLAN.md — русский (норм).

## Протокол замера/проверки (на каждом шаге)
- Нативный харнесс должен оставаться 54/54 (до деплоя).
- Деплой: обязательно явное имя назначения в `scp` + проверка md5 **установленной** либы
  (уже один раз молча встал старый APK из‑за `scp` без имени — см. AGENTS.md §130).
- Девайс-соак ~5 мин, затем: строка `flags:`, отсутствие `LOUD`/`HEAPCORRUPT`, пустой
  `death.log`, отсутствие новых tombstone, два скриншота (45 с и в конце).
- Метрики: (а) размер лога в байтах за прогон (сейчас 11.3 МБ); (б) fps из дельт
  `nativeUpdate t=<ms>` в `apphost.log` (CLOCK_MONOTONIC, мс); (в) время до меню по
  таймстампам logcat (в `apphost.log` времени нет).
- **Одна гипотеза = один прогон.** Не смешивать батчи в одном замере.

## Батч A — лог-мусор на горячем пути (самый дешёвый, доказан цифрами)
Из лога за 4.8 мин: `[rate]` 45068 строк, `[watchpc]` 44778, `T2+` 3747, `[call]` 511,
`r0=` 400 (кап), probe 131, rdump 166, diag164 330. Множители дороговизны:
`apphost.cpp:1047` `setvbuf(_IONBF)` ⇒ каждый printf = `write()` syscall;
`apphost.cpp:92` `Log()` пишет в **два** канала (файл + `__android_log_print`).

| # | Что | Где | Статус |
|---|---|---|---|
| A1 | `setenv("HARNESS_WATCHPC","250ab8")`; `WatchPC` капнут, а `WatchSP` — **нет** (шапка + скан 4096 Б стека с `CheckRange` на слово, на каждое попадание) | `apphost.cpp:1084`; `dispatch.cpp:8162-8191`; эмиссия `:10103` | PROVEN |
| A2 | `[rate]` printf на каждый update/render/input, **без флага** | `apphost.cpp:545-551` | PROVEN |
| A3 | `[netsock]`: 17 сравнений строк на **каждом** вызове импорта + безусловная печать для сетевых имён (комментарий сам пишет «Remove after triage») | `dispatch.cpp:956-962` | PROVEN |
| A4 | `ProbeDump2`/`ProbeDump3` **без капа** (в отличие от `ProbeDump` с `PROBE_N=500`) | `dispatch.cpp:7902`, `:8491` | INFERRED |
| A5 | `[env]` printf без капа для `stat/access/getenv/dlopen/open/fopen` | `dispatch.cpp:4727` | INFERRED |
| A6 | Безусловные printf в realgl | `realgl.cpp:635, 755, 899, 939, 404, 723, 941, 1038, 1041` | INFERRED |
| A7 | Безусловные printf на файловых операциях | `dispatch.cpp:2721, 2973, 3009, 3830, 3918, 3932, 3955` | INFERRED |
| A8 | `nativeUpdate` возврат логируется до 400 раз (кап работает — подтверждено счётчиком 400) | `apphost.cpp:889-896` | PROVEN |

## Батч B — per-call инфраструктура (при `HARNESS_DIAG=0` **пропускать целиком**, не просто молча)
| # | Что | Где | Статус |
|---|---|---|---|
| B1 | `OnSvc` trace: на каждом вызове импорта под `recursive_mutex` строится `std::string "#trace:"+name` + `unordered_map` probe (+ `threads_.find`/`svc_hist`) | `dispatch.cpp:869-946` | PROVEN |
| B2 | `CallJni` trace: на каждом JNI-вызове `recursive_mutex` + `std::to_string(fn)` + конкатенация + map (во многих `case` — ещё по одному `lock(mutex_)`) | `dispatch.cpp:6573-6595` | INFERRED |
| B3 | `EnvOn()` = mutex + `unordered_map<std::string,char>` + временная строка на каждый вызов; зовётся из malloc/free/write/recv/poll/cond_wait и **трижды за шаг размотки стека** | `dispatch.cpp:75-86`; `416,606,620,3041,3070,2327,2340,5018,5037,2429,2442,2473` | PROVEN |
| B4 | `PthreadMutex`: recursive_mutex + `mutex_map_` + `MutexNoteLocked` (`io_mutex_`+`mutex_owner_`) на каждый lock/unlock; `PthreadCond` — 2 map-find | `dispatch.cpp:4778-4874`, `4930-4960`; `dispatch.h:438-462` | INFERRED |
| B5 | `uniqcall` (`unordered_set::insert`+mutex) и `vacache` (mutex+map) **на каждом** Java→native вызове; `Enter()` — `g_tmu` lock + `g_threads.find` | `apphost.cpp:999-1009`, `603-621`, `493-534` | INFERRED |
| B6 | `AppHost_CallImpl`: 6-8 `strstr(name, …)` на каждый вызов; `TAPBEAT` блок компилируется в Android всегда (несколько `strstr` + копия вектора `words` на input) | `apphost.cpp:545,554,575,873,893,967`; `688-858` | INFERRED |
| B7 | `read`/`write`: **`fstat` syscall + io_mutex + fd_map lookup на каждом успешном I/O**, даже когда кап лога (200) исчерпан | `dispatch.cpp:3090-3121` | INFERRED |
| B8 | `poll`: `vector<pollfd>` + `fstat` на каждый fd для капнутого лога; `EnvOn(POLLDBG)` ×2 | `dispatch.cpp:2187-2243` | INFERRED |
| B9 | `realgl::Handle`: `std::string n(name)` + `snprintf` в `tag[128]` на **каждый** GL-вызов; на Android `Ensure()` делает `getenv("HARNESS_REALGL")` per-call (кэша нет, в отличие от PC-ветки) | `realgl.cpp:258-260`, `:129` | PROVEN |
| B10 | `strcmp/strncmp`: две временные `std::string` на вызов | `dispatch.cpp:1105-1106` | INFERRED |
| B11 | `PreCodeTranslationHook`: **`getenv` без кэша** (FIXPVR/POKE1187/BUILDERGATE/POKE180/`(POKE164\|\|DIAG164)`) — linear environ scan на **каждый транслируемый basic block**; `DIAG164` в бою включён | `dispatch.cpp:10043-10077` | PROVEN |
| B12 | `DIAG164`: host-call `PokeManagerFlag` эмитится на каждое исполнение `0x283B5C` | `dispatch.cpp:10077-10083`, `:8273` | INFERRED |
| B13 | Fallback-стаб: `stub_hits_[n]` (map под mutex) на каждый необработанный импорт + `[env]`-печать | `dispatch.cpp:4720-4736` | INFERRED |

### Батч A+B — СДЕЛАНО в коде (2026-10-17)
- Единый `DiagOn()` (`dispatch.h`/`dispatch.cpp`) + `DiagBootWanted()` в `apphost.cpp`
  (читает `HARNESS_DIAG`, в т.ч. из `abflags.txt` — defaults ставятся ДО его применения).
  Строка `flags:` теперь печатает `diag=` и `jitopt=`.
- **НАХОДКА (важно):** `HARNESS_PROBE=1a410c,fill` — это НЕ диагностика, а
  **поведенческий костыль** (пишет `"{}"` в пустой SSO-слот JSON на 0x1A410C, чтобы
  парсер не падал на Unexpected EOF; `dispatch.cpp:~8001`). Оставлен **безусловным**,
  рядом с RETSUCC/FASTMEM/SPURWAKE/REALGL. Побочно: `PROBE_N` ушёл в diag-блок, но у
  `ProbeDump` дефолтный кап = **8** (`dispatch.cpp:9756`) → дамп стал тише (8, не 500).
- Загейчено (только наблюдательное, поведение не меняется): A1 WATCHPC, A2 `[rate]`,
  A3 `[netsock]`, A4 `[memset]`, A5 `[env]`/`[stub]`, A6 realgl-печать (включая per-frame
  `glGetString`/`glGetIntegerv`), A7 `[files]` fopen/open/opendir, A8 `nativeUpdate` r0-лог,
  B1 OnSvc-trace, B2 CallJni-trace, B5 `uniqcall`, B7 `read`/`write` pipelog(+fstat),
  B11 `getenv` в `PreCodeTranslationHook` (кэш вместо per-block), B13 fallback-стаб,
  плюс `[poll]`, `[prop]`, `[sscanf]`, TLS ClientHello-печать, `[thread]`/`thread enter`,
  `[input]`, `[jni]`-мок-печати.
- B3 `EnvOn()` переписан: lock-free, без аллокаций (таблица по указателю-литералу); все
  19 площадок передают литералы (проверено grep-ом).
- НЕ тронуто (осознанно): B4/B6/B8/B9 (per-call `strstr`/`mutex`/`vector` вне диагностики),
  B10, B12 (эмиссия снята вместе с DIAG164), батчи C/D/E.
- **ЗАМЕР:** лог **11.3 МБ → 117 856 Б** (mid-pass, 5.5 мин) и **87 711 Б** (финал, 3 мин);
  pid стабилен в обоих, `flags: diag=off jitopt=none`, `HEAPCORRUPT`/`HEAPASSERT`=0,
  `death.log` пуст, новых tombstone нет; скриншот — **«LEVEL CLEARED!»** (уровень 1
  проходится с выключенной диагностикой). `LOUD`=3 — предсуществующий
  `[unwind] nesting 513 deep` (есть в `work/stab1..3.log`). Остаток лога — boot-диагностика
  `[realcall]`/`[realnew]`/`[realgl glTexImage2D]`/`[intl]`/`[regio]`/`[hello]` (~600 строк),
  НЕ тронут на этом проходе.

## Батч C — JIT/сборка
| # | Что | Где | Статус |
|---|---|---|---|
| C1 | ~~JIT по умолчанию `no_optimizations` в 4 местах; `HARNESS_SAFEOPTS` нигде не выставляется~~ → **СДЕЛАНО (2026-10-17):** дефолт `all_safe_optimizations`, выключатель `HARNESS_NOOPT=1`, единый хелпер `abr::JitSafeOpts()` (`dispatch.h`) на всех 4 точках + строка `flags:` (`jitopt=`). Замер (diag off, фиксированное 90-с окно на титульнике, 2 прогона на режим): none 6960/6938 тика vs safe 5668/5791 ⇒ **−17.5 % CPU**; нативный харнесс с оптимизациями 54/54, svc=948 | `dispatch.h`; `dispatch.cpp:5639,5764`; `apphost.cpp:507,1231`; `harness.cpp:612` | PROVEN |
| C2 | ~~`-DDEBUG=0` **включает** debug-assert (значение не важно: `#ifdef DEBUG` истинно), `ABORT_ON_ASSERT_FAILURE`=1, `ABORT` подменён хуком → `AbrHeapCorruptionHook(); abort();`~~ → **СДЕЛАНО (2026-10-17):** `DEBUG` из дефайнов **убран** (не `=0`). PROVEN по строке сборки: `dlmalloc.c` компилируется с `-DFOOTERS=0` и **без** `-DDEBUG` ⇒ ветка `#else` даёт `#define assert(x)` (no-op) + `#define DEBUG 0` (все поздние `#if DEBUG` по-прежнему ложны). Реальная порча по-прежнему падает через `erroraction` (RTCHECK/`ok_*`, C3) — файрвол не обесценен. Проверено: харнесс 54/54 svc=948, устройство 5 мин (md5 `d40bd295…` = установленной), pid стабилен, HEAPCORRUPT/HEAPASSERT/CHUNKD/USAGE_ERROR=0, `death.log` пуст | `host/CMakeLists.txt:47`; `dlmalloc.c:1454-1470`; `abr_heap_hook.h` | PROVEN |
| C3 | `INSECURE` не задан ⇒ `RTCHECK`/`ok_inuse`/`ok_pinuse` живут (постоянная цена, но и защита) | `host/CMakeLists.txt:47` | INFERRED |
| C4 | Dynarmic `ASSERT` жив в release (`DYNARMIC_IGNORE_ASSERTS` не выставлен) | `thirdparty/.../mcl/assert.hpp:28` | INFERRED |
| C5 | LTO/IPO выключены; флаги `-O3 -DNDEBUG -g` | CMake/flags.make | INFERRED |
| C6 | `USE_LOCKS=0` в dlmalloc ⇒ потокобезопасность целиком на внешнем bridge-mutex | `host/CMakeLists.txt:47` | INFERRED |

## Батч D — «firewall»/куча (что именно снимается — см. D1/D2)
| # | Что | Где | Статус |
|---|---|---|---|
| D0 | ~~БАГ~~ **СДЕЛАНО (2026-10-17):** ~~проверка головы чанка при возврате из карантина `(head & 3) != 3` даёт **ложные срабатывания**~~ → исправлено на `(head & 0x2) == 0` (CINUSE снят) `|| csize < 24 || csize > 16 МБ`, `csize` маскирует `~7` (FLAG_BITS), как `chunksize()`. Проверено на устройстве (md5 `612ad763…` = установленной): 5-мин soak, pid стабилен, **CHUNKD 0** (было 12/3 мин — все здоровые), abort 0, HEAPCORRUPT/HEAPASSERT/USAGE_ERROR 0, `death.log` пуст, новых tombstone нет; блоки снова освобождаются (утечки нет), канарейка снова осмысленна | `dispatch.cpp:674-705` | PROVEN |
| D1 | `ClampHeapWrite` = `recursive_mutex` + `std::map` на каждый memcpy/memmove/memset/strcpy/strcat/fgets/fread/sscanf + JNI-region. Коммиты-родители: `8a81005` «Overflow firewall (dlmalloc era)», `1bcd3b9` «Firewall: clamp fread/fgets…» — пластырь именно той охоты. План: **байпас по умолчанию** (функция и логика остаются, switch) | `dispatch.cpp:181-252`; вызовы `1030,1041,1071,1147,1165,2878,6761,6773` | PROVEN |
| D2 | `heap_sizes_` (`std::map`, node-alloc) обслуживается на каждом malloc/calloc/realloc/free. **Важно:** в бою у него нет guest-facing потребителя (`malloc_usable_size`/`dlsym` мы не эмулируем; `mspace_usable_size` зовётся только внутри `:390/:451/:527`). Потребители: ClampHeapWrite, sscanf-`%n`, регион-диагностика `alloc=` (кап 8), CHUNKD-верификация, liveness-проверка пула (`:7339`), heapwatch (off). Значит возможен отдельный follow-up: снести дерево целиком + заменить liveness-проверку пула и `%n`-клампы | `dispatch.cpp:394,455,531,520,525,573-589,7339` | PROVEN |
| D3 | `CheckRange` — линейный скан `mappings_` **без** last-hit кэша (в отличие от `CheckAccess`, `:254-296`); `mappings_` только растёт (`AddMapping` на каждый GuestMmap, никогда не удаляется). **Не снимать** — дать такой же кэш | `dispatch.cpp:746-764`; `:157`; `1122-1132, 1232-1243` | PROVEN |
| D4 | `quarantine_`/`quarantine_set_` + verify головы чанка (`CHUNKD`) + poison на каждом free | `dispatch.cpp:555-675`; `dispatch.h:284-292` | PROVEN |
| D5 | `GuestFree`/`GuestRealloc`: линейный скан `array_pool_` на каждый free/realloc | `dispatch.cpp:591-596`, `536-541` | INFERRED |
| D6 | `HeapWatchViolation` линейный скан `quarantine_` — мёртв в бою (fastmem on, heapwatch off) | `dispatch.cpp:5465-5522` | INFERRED |
| D7 | Остатки охоты: `ScanWP` (дёшев, early-return), `GAPWRITE`, `LOUD`-логи, `HeapCorruptionCount`, forge/snapshot-машинерия (`loginat_acct_snap_`, `HARNESS_WATCHPCSNAP`), `g_snap_region` | `dispatch.cpp:805-820, 228, 246, 5388-5459`; `harness.cpp:28` | PROVEN/INFERRED |
| D8 | Опционально (адресует тезис про «контеншен»): **lock-free префильтр** в `ClampHeapWrite` — если `dst` вне арены кучи, вернуть `len` без лока и дерева (сейчас лок берётся в первой строке `:182`, а проверка арены — позже, внутри `if (!size)`) | `dispatch.cpp:181-230` | PROVEN |

## Батч E — гигиена/доки (на рантайм не влияет)
| # | Что | Статус |
|---|---|---|
| E1 | 5 незаигноренных untracked в корне: `Ghidra_AngryBirdsRio/` 207 МБ, `angrybirdsriodebug.7z` 33.9 МБ, `libAngryBirdsRio.so` 6.5 МБ (тройной дубль), `rio_boot2.log`, пустой `0xd6f60` → в `.gitignore` | PROVEN |
| E2 | Доки расходятся с кодом: в «shipping junk» 10 флагов, реально активны 14 (нет `SPURWAKE`, `REALGL`, `RETSUCC`); «PORT LEDGER (do NOT ship)» перечисляет то, что ship'ится; строка `flags:` (`apphost.cpp:1162-1167`) показывает только 5 параметров | INFERRED |
| E3 | `work/ARCH.md` всё ещё описывает bump-аллокатор 1 ГБ / heap `0x10000000–0x50000000` | INFERRED |
| E4 | `assets/seeds/highscores.lua` = 0 байт; `cacert.pem` продублирован (`assets/seeds/` ≡ `assets/data/certificates/`, md5 `9d8c2e9a…`), +256 КБ в APK | PROVEN |
| E5 | Деплой-цепочка — только проза в AGENTS.md; отдельного скрипта с md5-проверкой нет | PROVEN |
| E6 | `host/gen_apphost.py` зависит от gitignored входов (`work/jni_signatures.txt`, `work/apk_prod/smali`), вывод `apphost_gen.inc` закоммичен | INFERRED |

## НЕ трогать
- `CheckAccess`/`CheckRange` как **контракт** (дикий guest-указатель → чистый halt, а не SIGSEGV хоста).
- `HARNESS_RETSUCC`, `HARNESS_SPURWAKE` — это **поведенческие** костыли, не диагностика.
  Снятие = другой билд/отдельный эксперимент (реальный fail-path «stalls on Fox»; untimed cond_wait
  иначе даёт 110/abort). `HARNESS_REALGL`, `HARNESS_FASTMEM` — нужны.

## Порядок работ (по батчам, каждый — отдельный замер)
1. **A** (лог) → соак: ожидаем лог <1 МБ, fps/поведение без изменений.
2. **B** (per-call инфраструктура при `DIAG=0`) → соак.
3. **C1** (JIT-оптимизации) → A/B `SAFEOPTS on/off`; затем флип дефолта.
4. **C2** (DEBUG=0) → нативный харнесс + соак.
5. **D1** (ClampHeapWrite off) → соак/замер; при сомнениях D8 (префильтр) как компромисс.
6. **D2/D5/D3** (дерево/пул/кэш CheckRange) — по остаточным цифрам.
7. **E** (гигиена/доки) — попутно, без замеров.
8. По завершении — новый тег (по отдельной команде).

## Открытые вопросы к пользователю
1. ~~Порядок~~ → решено: A, затем C1.
2. ~~RETSUCC/SPURWAKE~~ → решено: не трогаем, отдельный эксперимент (см. Решения п.7).
3. ~~JIT-дефолт~~ → решено: двухшагово, см. Решения п.8.
4. ~~Язык PLAN.md~~ → решено: русский.
Пока открытых вопросов нет.
