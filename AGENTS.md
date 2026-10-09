# AGENTS.md — AngryBirdsRio arm64 port (dynarmic). FULL STATE DUMP.

## 0. How to work here (rules learned the hard way)
- RW ONLY inside this folder (+ /tmp freely). NEVER install anything system-wide.
  If a system install is truly needed: STOP and ask the human (they install).
- Downloads must be hash-verified (official SHA256 / sdkmanager's built-in checks
  / `git fsck`). See §1 for what's already fetched.
- **Docs first, code second.** Never use an API from memory: open the header
  (Dynarmic, NDK, ELF) and verify signatures/layouts before writing code.
- **No mental hex arithmetic.** Every address computation goes through code
  (python/shell). Past head-math errors burned hours. When printing addresses
  for humans, prefer decimal + code-verified hex; 0x1AECDC vs 0x1AEDC lookalikes
  caused a multi-hour wild goose chase.
- Distinguish PROVEN (machine-verified) from INFERRED in every claim.
- `jev` is cheap: `jev_noul` for competing hypotheses at forks,
  `jev_verify`/`jev_gate` before reports and milestones.
- Python with third-party libs goes into a project-local `.venv` (none needed yet).

## 1. Layout (all portable, nothing outside this dir except /tmp)
- `com.rovio.angrybirdsrio.2676.apk` — ORIGINAL, DO NOT MODIFY (sha256 of
  lib/armeabi-v7a/libAngryBirdsRio.so = 980fad19…; verify if in doubt).
- `tools/` — SDK: build-tools 35, platform android-35, platform-tools (adb 37),
  NDK r27 (27.2.12479018), apktool jar, `sdkmanager.sh` + `avdmanager.sh`
  wrappers (stock scripts break on SPACES in path; wrappers call java directly
  with quoted classpath; avdmanager needs BOTH -Dcom.android.sdklib.toolsdir
  AND -Dcom.android.sdkmanager.toolsdir).
- `thirdparty/dynarmic` — azahar-emu checkout (merryhime is dead), submodules
  complete, `git fsck` clean. A32 frontend only.
- `thirdparty/boost` — headers only, tarball SHA256-verified vs boost.org page
  (`cc4b893a…`). `BOOST_ROOT` = thirdparty/ itself (contains boost/version.hpp);
  also set `Boost_INCLUDE_DIR` (FindBoost is half-removed in CMake 4.4 and fails
  under NDK cross-compile without it). `DYNARMIC_USE_BUNDLED_EXTERNALS=ON`.
- `work/abr` — apktool decode (framework seeded in work/framework/1.apk).
- `work/lib/armeabi-v7a/libAngryBirdsRio.so` — guest binary copy for harness.
- `work/*.txt` — inventories: `inventory_imports.txt` (338 undefined imports),
  `inventory_jni.txt` (63 Java_ exports), `jni_signatures.txt` (all 63 signatures
  extracted from smali — hot path is primitives/arrays only),
  `thunk_inventory.txt`, `funcs_1a.txt` (Ghidra functions, NOTE: Ghidra's
  address space may be shifted vs file offsets — DO NOT trust blindly),
  `dis_1aebc0.txt` (same caveat), `decomp_killer.txt`, `xrefs.txt`.
- `work/scripts/` — Ghidra scripts (ListFuncs, DisRange, DecompileAt, XrefsTo).
  Ghidra itself: /opt/ghidra 12.1.2, run headless with JAVA_HOME=java-21 +
  HOME=work/fakehome (keeps ~/.ghidra out of the system).
- `work/ghidra_proj` — Ghidra project (kept, has analyzed import).
- `work/harness.log`, `work/decomp.txt` (Lifeline-era? no — ABR decomp not made).
- `host/` — the arm64 host project (CMake). Targets: `smoke_a32`, `cbnz_test`
  (scratch micro-tests), `distv` (Dynarmic-based ARM/Thumb disassembler for the
  game file — TRUSTED decoder), `guest` lib (elf_loader, dispatch), `harness`.
  `host/build-native` (x86_64), `host/build-android` (NDK arm64 — libdynarmic.a
  PROVEN to build there).
- `out/` — final APK goes here (empty so far).
- /tmp scratch (survives): /tmp/abr3 (guest .so copy), /tmp/eng_*.so (patched
  COPIES for experiments — originals never patched), /tmp/zero.log.

## 2. Guest binary facts (ALL machine-verified, do not re-derive)
- ET_DYN ARM32 LE, bias 0. 2 PT_LOADs: [0,0x653D1C) R-X and file-off 0x654CC0
  → vaddr 0x655CC0 filesz 0x1CFB0 memsz 0x978B0 RW (ends 0x6EE000). No PT_TLS,
  no TEXTREL. BIND_NOW. 55 INIT_ARRAY entries (54 runnable after null-skip).
- Reloc types ONLY: RELATIVE 16807 (first), ABS32 3389, JUMP_SLOT 1141, GLOB_DAT
  194. Numeric: ABS32=2, REL32=3, THM_CALL=10, GLOB_DAT=21, JUMP_SLOT=22,
  RELATIVE=23. DT tags numeric: REL=17@0x29580 (20390 entries), RELSZ=18,
  JMPREL=23@0x512B0, PLTRELSZ=2 (9128 → 1141 entries). NO packed relocs
  (0x6000000D/E/F absent), no RELA. Full DT verified programmatically.
- Game libs: engine needs ONLY libandroid/libGLESv2/libc/libm/libstdc++/libdl/
  liblog. libjs+libadcolony (ads, dead) are droppable — Java disables AdColony
  itself if .so missing. Engine dlopens ONLY system libs (no libjs).
- Java loads "AngryBirdsRio" (Globals.loadLibraries ← Gen.library_name) +
  "adcolony" (graceful-degrade). Audio: Java→native `nativeMixData(J[BI)`
  (engine fills buffer). EGL fully Java-owned (GLSurfaceView+EGLContextWrapper);
  engine issues GLES2 calls only (no egl/ANativeWindow imports). No OpenSL.
- Threads at runtime: GL thread (nativeRender), UpdateThread
  (nativeUpdate/nativeInit), AudioThread (nativeMixData). Host will need
  per-thread Jit sharing GuestMemory.
- Java entry: com.rovio.fusion.App (landscape, singleTask). targetSdk 26,
  minSdk 16. Ad/billing SDKs dead on modern Android — degradation accepted.
- Data imports needing guest slots: __stack_chk_guard (canary, odd-random),
  __errno (func→slot), __sF/_ctype_/_tolower_tab_/_toupper_tab_ (zeroed for now;
  real 2014-era ctype tables TODO), getauxval.
- NO mrc-p15/TLS access. TEXTREL absent. All good.

## 3. Host design (implemented, in host/src/guest/)
- 4GB reservation (mmap PROT_NONE+NORESERVE); modules mapped at linked addrs.
- Layout: modules [0, ~0x6EE000); heap bump allocator @0x10000000 (256MB,
  HARNESS_HEAPBASE env can relocate for experiments); mmap arena from
  0x70000000 up; trampolines @0xF0000000 (64KB, `svc #idx+1`, slot0=`svc #0`
  HALT); JNI table @0xF0010000 (reserved, bridge TODO); guest data @0xF1000000
  (canary+0, errno+0x100, misc slots); stack @0xFE000000+8MB, SP top 0xFE800000.
- SVC dispatch: swi 0=HALT (also hijack-completion, see below), 1..N = imports
  (trampoline order = relocation order), 0x100000+i = JNI function #i (256
  loader-emitted trampolines -> JNIEnv table @kJniTableBase; JNIEnv* @guestdata
  0x2000). Implemented JNI: 164/168/169/170 strings, 24 IsSameObject,
  171/184/192 byte-arrays (opaque handles == buffer VAs); rest = LOUD halt
  (discovery mode).
  Ret() = r0/r1 result + pc=lr&~1 + SetCpsr(USER + T-from-LR + preserved NZCV).
  **RULE: SVC trampolines never change state; Ret restores caller state.**
- Memory callbacks only (NO fastmem yet — debuggability first; enable later).
  All ranges tracked; violations halt cleanly. Host NEVER segfaults on guest
  input (CheckRange on bulk thunks; remaining gaps known, see §6).
- Heap: bump + size map (realloc COPIES now). free = no-op (leak, bring-up OK).
- pthread: REAL mutex/cond/key/TLS maps with lazy creation (Bionic allows
  static-init use); RECURSIVE host mutexes. cond_timedwait capped 5s.
  pthread_create/join/detach = LOUD halt (not implemented).
  pthread_once = CONTROL-FLOW HIJACK (no nested Run): redirect pc to routine
  with lr=HALT, on HALT mark done + restore saved regs (r0=0) + resume.
  Recursion → LOUD + treat done. Implemented, built, not yet triggered.
- LDREX/STREX via CAS emulation (single-thread exact).
- libm: ~60 funcs forwarded to host libm with softfp marshalling
  (host/src/guest/math_thunks.inc). HARNESS_HEAPBASE/HARNESS_SKIP/HARNESS_DEFER/
  HARNESS_STEP/HARNESS_FULLTRACE/HARNESS_WP/HARNESS_WPA/HARNESS_TRANSLOG envs
  for experiments (see harness.cpp). stdout is _IONBF. Watchdog alarm(300).
  Heartbeat thread samples pc (RACY values — liveness only, see audit #13).

## 4. Dynarmic API facts (verified in headers/sources, NOT assumed)
- Entry: even PC + `SetCpsr(0x30)` Thumb / `0x10` ARM (from Dynarmic's OWN tests).
  PC LSB ignored for state. (Found via Dynarmic's tests/A32 — docs-first win.)
- `Jit::Regs()` = 16 entries ONLY (r0-r15). CPSR via `Jit::Cpsr()`/
  `SetCpsr()` — NEVER `regs[16]` (past-the-end garbage caused FIQ-mode bug).
- `SetCpsr()` updates upper_location_descriptor (T/E/IT) — safe to call;
  `Reset()` must NOT be called from callbacks; `ClearCache()` halts if in callback.
- `LDR pc, [mem]` interworks to Thumb on odd values (micro-test PROVEN r0=1,r1=2).
- `ldr r0, [pc, rN]` computes correctly (micro-test PROVEN r0=0x102C).
- Regs() reads are ACCURATE (self-reporting-pc micro-test: guest-stored pc/r0/r1
  match host reads). Ring/wpr register values TRUSTED.
- PreCodeTranslationHook fires per translated instruction (used for TRANSLOG).
- printfs inside callbacks are fine; stdout unbuffered to survive _exit().
- Backend asserts are ON in our build (abort on coproc-without-handler etc.).

## 5. Current status: init 53/54 + #22 PARKED (wall resolved as dead-by-construction)
SYMPTOM (was deterministic, all runs): init#22 (entry stub 0x5885C = `b 0x1AEB24`)
died: indirect call through value 0xFFFF0FA0 loaded from VA 0x672010; r0 holds
it at a `blx r0`, lr = post-blx. svc counts identical across runs (640).
MECHANISM (every link machine-verified): TRUE site `ldr r0, [pc, r2]`
(0xE79F0002, file-verified) + `blx r0` (0xE12FFF30) in init#22's flow; with
r2 = 0x4C332C (literal, file-verified, no reloc) the effective address is
exactly 0x672010 (patch experiments: literal+4 moves death 0x672010->0x672014
AND value FFFFA0->FFFC0 — CAUSALITY PROVEN). *(0x672010) = 0xFFFF0FA0
FILE-ORIGINAL (verified 2026-10-09 via struct parse: no writer ever needed).
NO reloc targets it (python + readelf agree: 43 REL in [0x672000,0x672100),
ALL type 23 RELATIVE, zero at 0x672010; zero JMPREL in range). Regdiff proves
NOTHING ever writes [0x672000,0x672100) during the run.
SUBSYSTEM (subagent #2 static autopsy, verified 2026-10-09): rcs::analytics
singletons (typeinfo at 0x6037E0/0x603800/0x603840 = Parameter/Event/
StoredLogs) — NOT GooglePlayPaymentProvider (that label was 0x32510 bytes of
nearest-symbol coincidence, refuted by code). Chain: 0x1AEB24 (once-guard at
0x673279) -> 4x operator new (32/48/44/40B) -> first ctor 0x1AEC40 -> 0x1AECAC
-> `blx r0 @0x1AECE0` -> *0x672010. At least 10 twin sites share the idiom;
first twin kills, rest unreachable. Region 0x672000+ is .data (NOT exidx/extab:
extab @0x522998 has zero FFFF0FA0/FC0 words), a concat of float consts +
per-subsystem tables; slot sits in a marker-word gap, never a pointer.
VERDICT: dead-by-construction — no loader action can fabricate the missing
pointer (RELATIVE is no-op at bias 0; no filler found in 5 scan methods).
DECISION (done): PARK #22 in harness (default skip + LOUD log, HARNESS_FORCE22=1
overrides for experiments). With park: 53 run + 1 parked = all green.
Revisit ONLY if nativeInit needs analytics singletons (unlikely: analytics is a
dead subsystem, game degrades without it).
- JNI first blood (2026-10-09): HARNESS_JNI mode calls guest Java_* exports with
  fake JNIEnv. nativeGetPossibleOrientations @0xc14b4 (ARM, pure bitmask over a
  global array, ignores env) returns r0=10 clean via HALT (hr=UserDefined1).
  +1 silent svc inside (likely guard-acquire no-op; TODO identify when needed).
- JNI inventory (subagent #3 static scan, 2026-10-09, tool-verified): all 63
  exports are ARM-entry (st_value LSB even 63/63). Only 4 use JNI directly:
  nativeConfig (169 GetStringUTFChars + 170 Release), nativeLoadFromUrl (same
  pair, bx tail-call), paymentFinished (IsSameObject #24 x3), nativeMixData
  (184 GetByteArrayElements + 192 Release, bx tail-call). Other 59 delegate to
  C++ helpers (transitive JNI out of scope). No runtime-computed table indices.
  ~70 vtable-idiom indirect calls classified NON-JNI (bases are objects, small
  offs). Bridge must treat `bx Rx` to JNI entries as calls (lr = return).
- nativeConfig BLOCKED (2026-10-09): dies BEFORE any JNIEnv call near
  0x2FA344-0x2FA38C: second-stage slot 0x6747D0 (.bss, NO reloc even at +-4)
  yields r1=0x464C457F ('\x7FELF' bytes) -> BAD read [r1+24]=0x464C4597
  (unmapped gap). Stage1 slot 0x6703CC = __stack_chk_guard (GLOB_DAT, file 0).
  Subagent #4 (static prologue flow) on it. NOTE: StepRun stepper ABORTS if it
  steps past a callback halt (Dynarmic coproc assert) — use Run()+TRANSLOG for
  JNI forensics until StepRun learns halt-awareness.
- RESOLVED 2026-10-10 (subagent #4): crash was missing-JavaVM-init — harness
  never called JNI_OnLoad @0xC2DB4 (sole writer of 0x6747D0). Fix: HARNESS_JNI
  now runs OnLoad (HARNESS_ONLOAD addr, HARNESS_NOONLOAD=1 skips) with mock
  JavaVM (GetEnv #6 / Attach #4 -> our JNIEnv). Bridge grew discovery-driven:
  FindClass, GetMethodID #33/GetStaticMethodID #113 (cookies with names),
  refs 21/22/23/25/227/228 (no-op/same), exceptions 15/17, CallStaticObjectV
  114/115/116 (fresh mock objects), arrays 171/184/192, strings 164/168/169/170.
  Real string-search family implemented (memchr/memrchr/strchr/strrchr/strstr/
  strpbrk/strspn/strcspn/strtok(_r)/strtol(l)/strtoul(l)/atoi/atol/atoll).
- RESOLVED 2026-10-10: pthread_once-hijack CPSR bug (handler SVC mode leaked
  into routine/resume -> Undefined on pop.w) — hijack now forces USER+T like
  Ret(). ldr/add-pc ARM switch idiom emulated in ExceptionRaised (Run mode
  reports BLOCK start = the ldr; match ldr+add pair, force USER+ARM: JIT falls
  through Thumb `bx pc` pads without switching). get_globals_fast tail-jumps
  through pad+dispatch into pthread_getspecific PLT — legitimate, now works.
- RESOLVED 2026-10-10/11: FIRST THROW ROOT-CAUSED — void-JNI r0 lie. Game checks
  DeleteWeakGlobalRef's r0 at 0x267FD0 (non-zero = error -> "Java method threw
  an exception" throw). Our RetVoid preserved env (non-zero); Dalvik leaves 0
  (game works on device => device sees 0). Fix: ALL void JNI thunks return r0=0
  (Release*/Delete*/ExceptionClear). Result: 131K-throw nest VANISHED (svc
  1.7M -> ~110 for the phase); the rethrow back-edge (subagent #6: pad always
  aborts, true fix = kill original exception) became moot.
- MILESTONE 2026-10-11: nativeConfig RETURNS CLEANLY (rc=0, r0=0, 0 LOUD/BAD).
  String model: jni_strings_ registry; String-returning static methods produce
  registered jstrings (getPathToFileCacheDirectory -> real files-dir path);
  GetStringRegion/UTFRegion implemented (5th arg from guest stack).
  Regression: inits 54/54 svc=948 (unchanged), orientations r0=10.
- INCIDENT 2026-10-11: harness.cpp main() destroyed by a careless python bulk
  delete (510 lines, wrong match anchor). Reconstructed from session knowledge
  (~500 lines: init loop w/ SKIP/DEFER/park/STEP/ring/regdiff/bt, OnLoad, JNI
  sequencing via new RunOneJni helper). Behavior VERIFIED identical on 3
  baselines (inits 54/54 svc=948 + PARKED line; orientations r0=10;
  nativeConfig rc=0 0 LOUD/BAD). Lesson: never bulk-delete without a backup
  copy + match-count check; prefer small exact edits. Dropped in
  reconstruction: /proc maps dump, distinct-bucket JNIBT part (buckets kept).
- MILESTONE 2026-10-11: nativeInit RETURNS CLEANLY in sequence
  (config -> init(0x320,0x1E0), HARNESS_JNI2, rc=0). Init issues first GLES
  calls (glFrontFace/Viewport/UseProgram/DepthMask/DepthFunc/CullFace/
  BlendFunc/BlendEquation/Enable — all stubbed, 0 LOUD/BAD).
  NEXT: resize/update/render sequence -> real GLES thunks milestone.
- FILE BRIDGE (2026-10-11, implemented): guest files-dir prefix maps to
  <project>/work/files (HARNESS_FILES overrides; derived from .so path).
  FILE*/DIR* = guest-heap handles, fds >= 100, stdio fds forwarded (write 1/2
  -> [guestout/err] log). Implemented: fopen/fclose/fflush/fread/fwrite/
  fseek/fseeko/ftell/ftello/rewind/feof/ferror/clearerr/fileno/fgets/fputs/
  fputc/getc/fdopen/tmpfile/open/close/read/write/lseek/writev/stat/fstat/
  access/mkdir/unlink/remove/rename/rmdir/chmod/getcwd/fsync/opendir/
  readdir_r/closedir. /dev/null|zero|urandom -> host same; /dev/alarm ->
  /dev/null (timer ioctls stubbed). Others LOUD-fail (discovery). stat uses
  NDK-r27 ARM layout (2014-compat by behavior). HARNESS_JNI=list + JNIBT kept.
- WALL (2026-10-11): update throws via 0x2A6E4C raiser naming
  `rcs::Consents::Impl::getConsents` (std::function signature in error
  descriptor @0x66644C). Nearby rodata: networkId/consent/version/
  NOT_ANSWERED/consentId/properties. No consent surface in JNI/smali
  (pure native rcs framework). Hypotheses: (a) empty provider invoked
  (Java registers it on device; nothing sets it in harness); (b) stored
  state NOT_ANSWERED with no UI to answer. Speed ruled out (full sequence
  runs in 0.3s without ASan). Subagent on the validation-loop field check.
- SUBAGENT #7 REPORT (2026-10-11, verified pieces): FileInputStream::open
  @0x2C8388 (only success return; NULL/slash always throw FileException,
  typeinfo 0x65ED5C si_class). Downstream: readExact (EOF short-read OK),
  measure, parse @0xB8E74 -> BINARY deserializer 0x2FC420 (empty range
  returns success!) + DecryptInputStream/EncryptOutputStream (AES S-box
  @0x4C6BE0). Writer twins exist ("wb" @0x2C9138, "ab" @0x2C9514, 6 sites,
  save-on-event). No defaults anywhere (registry @0x72980 only registers).
  Landing pads unwind to data (never fire). Empty file DID reach guest
  (fopen -> handle, PROVEN in jniempty.log) yet crashed identically ->
  placement-flaw theory REFUTED by main thread.
- EMPTY-FILE RUN (2026-10-11): fopen ok -> 4240 blocks through measure/read,
  decrypt-setup 0x2FBACC, deserializer, game logic (0x27/0x39) -> throws at
  SAME 0x2C8574 builder. So empty parses but fails downstream (nil-data?).
  Subagent #7 follow-up: stream header format, key location, minimal blob.
- RESOLVED 2026-10-11: Thumb `add Rd,pc` reads RAW insn+4 (no &~3 align).
  Proven: pools+raw-pc land EXACTLY on GLOB_DAT slots (0x670838 _ctype_,
  0x6703CC guard); aligned-pc lands on dead bytes. Dynarmic GetRegister(PC)
  (raw, a32_ir_emitter.cpp) is CORRECT — file/reloc analysis looked clean
  because the mechanism was right; only table data was missing.
- RESOLVED 2026-10-11: real BSD ctype tables (InitDataImports: 257B + 2x256
  u32 tabs, EOF slot, slots patched via pending_data). Number parser advanced
  (space-skipping works). Inits unaffected (54/54).
- StepRun SOUNDNESS (2026-10-11): stepper cleared real callback halts and once
  faked a clean render pass. Fixes: stop_step_ flag set at all 38 UD2/3 halt
  sites (mechanical); StepRun/init-loop break on it; cap-exhaustion now
  reports NOT-clean (was faking UserDefined1). HARNESS_JNISTEP=N steps calls
  >=N; HARNESS_STEPCAP sets cap. NOTE: stepping perturbs wall-clock branches
  (slow) — Run mode is ground truth; step only for micro-forensics.
- RESOLVED 2026-10-11: GLOB_DAT data slots hold &variable (standard), and the
  game double-derefs (slot->[var]->use, proven at 0x45C174 + guard chain).
  InitDataImports allocates pointer cells (*V=table/canary) and patches slots
  with &V. Killed the 0x20202021/0x464C45AD wild-pointer faults. Subagent #8.
- NOTE (2026-10-11): getspecific(0x2000000 garbage key) is HARMLESS slow-path
  (miss -> calloc emergency globals -> setspecific -> proceed), NOT the throw
  trigger. Proven by ordered SVC log + TRANSLOG throw site (jev_verify:
  verified/auto). Do not chase key provenance again.
- JEV BATCH 2026-10-11 (all verified): add-pc raw (auto, 1.0); nest-data
  (auto, 0.97); void-r0 (review, 0.76/0.24 — weak point is the inferred
  Dalvik-leaves-0 premise, causality itself is solid); BSD-ctype (review,
  0.68/0.25 — table works for tested bits, high bytes + EOF value unproven).
  Action: treat Dalvik-0 and ctype-high-bytes as tunable assumptions. The throw follows later via the
  0x2A6E4C raiser after the 0x2AB7xx/0x476xxx validation ping-pong.
- TOOLING (2026-10-11): shell tool mangles quoted absolute paths containing
  spaces+non-ASCII ("Рабочий стол"); use workdir=project dir with RELATIVE
  paths in commands. HARNESS_TRAMPMAP=1 dumps idx->name->trampoline VA.
  InitDataImports now allocates pointer cells (*V=table base / canary) and
  patches slots with &V. Killed the 0x20202021/0x464C45AD wild-pointer faults
  (table bytes were being read as pointers). Subagent #8 diagnosed.
- GLES RECON (2026-10-11): 68 gl* imports, ZERO egl* imports (engine never
  touches EGL — Java owns context/surfaces, confirmed). Host has NVIDIA
  GLESv2 + mesa libGLESv2.so.2 + GLES2/GLES3/EGL headers; no Xvfb (forbidden
  to install). Headless render tests feasible later via surfaceless EGL
  (EGL_MESA_platform_surfaceless / EGLDevice, no X needed). Thunk approach:
  forward to host GLESv2 with H() pointer marshalling (guest mem is
  host-mapped, buffers direct; string arrays need rebuild). 68 funcs scaffold
  AFTER update passes (untestable before). Subagent #9 on validation loop.
- CORRECTION (2026-10-11): the 131K x 0x0C0F8C stack words are DATA, not
  return addresses — TRANSLOG proves 0x0C0F8C never translates in the update
  run. Each nesting level STORES the pad address (per-level exception/unwind
  records, ~64B). Subagent #6 retried with this correction + first-throw site
  (0x2AB6B4 canary fail, guard slot stable across 1.45M reads).
- CYCLE ANATOMY (2026-10-10, proven): guest-stack scan at death finds return
  address 0x0C0F8C x131024 of 131109 code ptrs (single site!). 0x0C0F88 =
  `mov lr,pc; b __cxa_throw` in @0x0C0F58 (throws iff current_exception() =
  PLT 0x53914 result non-NULL). First throw is elsewhere (0x26806C region,
  msg "Java method threw an exception" from rodata 0x5DDFE8, via throw PLT
  second word 0x53710); the other 131023 are rethrows from 0x0C0F88. Catch pad
  @0x0C0F8C (begin_catch in-module, invisible to SVC hist) forks on r4==2 but
  BOTH forks end in tail-throw (0xC0F8C-path via 0xC100C `mov lr,pc; b`);
  __android_log_print import NEVER fires (0 SVCs) so neither logging fork runs
  — yet nesting persists, i.e. re-entry bypasses the pad's logging calls.
  All C++ runtime is in-module (throw/begin/end/personality/get_globals);
  only Find_exidx was an import (now real: loader parses SHT_ARM_EXIDX,
  prel31 bit30-sign verified, 18184 entries, exact fn matches — behavior
  UNCHANGED, same digits). Subagent #6 hunting the back-edge (pad ->
  re-entry into 0x0C0F58) + r4-fork semantics.

## 6. Known gaps / TODO (audited + observed)
- Bulk thunks missing CheckRange: memcmp/bcmp (FIXED), strcpy-family (FIXED w/
  bounded scans), clock/gettimeofday (FIXED to 32-bit layouts), PthreadKeyCreate/
  timedwait-H (check!), strcasecmp (FIXED real), setjmp/longjmp (stubs — risky
  for exception paths), dlopen/dlsym/open/stat/read/getenv/sysconf (stubs
  returning 0 — divergence surface), qsort (no comparator support — needs the
  once-style hijack), pthread_cond_timedwait_monotonic (FIXED via alias),
  GuestMunmap leaves stale mappings, GuestMmap FIXED-range can overlap heap.
- Real ctype tables (2014-era Bionic layout) for inline macros.
- C++ exception unwinding through translated code (personality/exidx at runtime;
  __cxa_throw is DEFINED in-module — future milestone, will need bridge care).
- Real threads (pthread_create + per-thread Jit + errno/TLS slots).
- fastmem enablement (after correctness), code-cache management.
- Heartbeat Regs race (liveness-only use now; ok).
- stdout _IONBF set; logs must survive _exit (watchdog path).

## 7. Diagnostics cheat-sheet (all in-harness, no device needed)
- HARNESS_STEP=22 (+FULLTRACE=1): per-step (pc, mem-word, r0-r3, r12, lr).
  NOTE: pc labels verified trustworthy ONLY after the mem-word cross-check
  harness (mismatches: 0). If pcs ever disagree with file bytes again, suspect
  reporting lag before suspecting the file (sha256 pinned: 980fad19…).
- HARNESS_WP=<hexval>: value watch (writes/reads/bulk). HARNESS_WPA=<hexaddr>:
  address watch (reads+writes). HARNESS_TRANSLOG=1: translated pcs.
- HARNESS_SKIP/HARNESS_DEFER: init isolation/ordering probes.
- HARNESS_HEAPBASE: heap relocation probe (0x10000000 default).
- distv (host tool): Dynarmic-decoder disassembler — TRUSTED for code regions.
- Ghidra headless works but ITS ADDRESS SPACE MAY BE SHIFTED vs file offsets
  (observed conflicting bytes once) — cross-verify with xxd/python, never trust
  blindly. Project kept at work/ghidra_proj.
- /tmp/eng_*.so = patched COPIES for experiments (never patch originals).

## 8. Next milestones (REVISED 2026-10-11 eve — §5 has the full log)
1. DONE: init#22 parked (HARNESS_FORCE22=1 overrides).
2. DONE: JNI bridge live (63 exports mapped; mock JNIEnv/JVM tables; strings/
   arrays/objects/methods/int-methods; orientations/nativeConfig/nativeInit/
   nativeResize return clean).
3. ACTIVE WALL: nativeUpdate throws (consents-related raiser 0x2A6E4C, see §9).
4. NEXT: real GLES thunks (68 gl* fns, zero egl* — Java owns context; host has
   NVIDIA GLESv2 + headers; surfaceless EGL for headless tests; MCSM does the
   same set). Consider RegisterNatives (single JNI_OnLoad) instead of 63
   exports at packaging (MCSM precedent; jni_signatures.txt becomes the table).
5. THEN: audio (AudioOutput.nativeMixData needs engine audio singleton —
   standalone call dies NULL+0x6C; needs full boot context), input, sensors.
6. THEN: Android shell + packaging (guest→assets/guestlib, host→lib/arm64-v8a,
   drop x86/ads) → device logcats.

## 9. State dump 2026-10-11 eve (everything since §5's last lines)
- THREADS LIVE: pthread_create spawns host std::thread + private Dynarmic Jit
  (shared GuestCallbacks/Runtime/mem; ActiveJit() routing; TLS/errno per tid;
  join/detach/self/yield; recursive_mutex everywhere after gdb-proven
  unordered_map races; GuestStr thread_local scratch (was heap-corrupting);
  _exit() at all post-Runtime exits (exit-UAF proven by ASan); ASan episode
  reverted (CMakeLists clean). Worker tid=2 (start 0x38C048) bootstraps TLS/
  event-poll loop concurrently. inits 54/54 svc=948 unchanged.
- STEP SOUNDNESS (all fixed after faked-clean incidents): stop_step_ at 38
  UD2/3 sites; StepRun/init-loop break on it; cap-exhaustion = NOT-clean;
  HARNESS_JNISTEP=N steps calls>=N; HARNESS_STEPCAP sets cap. Run mode is
  ground truth (stepping perturbs wall-clock branches).
- DIAGNOSTICS ADDED: HARNESS_TRAMPMAP (idx->name->tramp VA); HARNESS_JNIBT
  with distinct-bucket scan; HARNESS_HEXDUMP addr,len; HARNESS_HEXFIND needle
  (heap scan, bump-no-reuse persists); HARNESS_CALLMAX/JNIMAX/SVCHIST;
  HARNESS_JNI=list + JNI2 + JA2/JA3 + JSTR + JBARRAY/JSTK(stack args + arrays);
  per-thread log tags ((T2+), HARNESS_THREADLOG=1 for workers); glog printf
  expansion (%s/%d/%x/%c/%%, ARM APCS r3+stack).
- FILE BRIDGE (all in dispatch.cpp): files-dir prefix -> work/files
  (HARNESS_FILES overrides; NOTE: harness must run with project dir as CWD —
  paths derived relatively). fopen/fclose/fflush/fread/fwrite/fseek/fseeko/
  ftell/ftello/rewind/feof/ferror/clearerr/fileno/fgets/fputs/fputc/getc/
  fdopen/tmpfile/open/close/read/write/lseek/writev/stat/fstat/access/mkdir/
  unlink/remove/rename/rmdir/chmod/getcwd/fsync/opendir/readdir_r/closedir +
  pipe + real poll() (guest->host fd translation, 5s cap). fread/fwrite take
  handle from a3 (a0-keying bug returned 0 silently — fixed). Fake FILE* =
  96B guest blocks with Bionic _file@+14 = guest fd (inline ldrsh reads!);
  fileno() returns stored guest fd. fprintf/fscanf LOUD (varargs).
  stat = NDK-r27 ARM layout (risk noted). /dev/alarm -> /dev/null.
  work/files seeds: highscores.lua/settings.lua/bi_data.lua/fusion.registry
  (empty ok) + extracted data/ tree (70MB APK assets). Game WRITES real
  content (bi_data 32B, registry 16B, .tmp+rename works; fsync/rename verified).
- ASSET BRIDGE: APK zip central-dir parser (CAREFUL: csize@20/usize@24/nlen@28
  — off-by-4 bug shipped 38B once), stored+deflated (zlib, link z), getBuffer
  model (handle IS guest buffer), getLength64, AAssetManager_fromJava.
  HARNESS_APK override. variantconfig.json = 1904B stored, byte-verified.
- JNI ADDITIONS: IsSameObject, refs 21/22/23/25/227/228, exceptions 15/17,
  CallStaticObject(V/A) + CallObject(V/A) 34/35/36/114/115/116 (String sigs ->
  registered jstrings; getPathToFileCacheDirectory -> real path),
  CallStaticIntMethod 129/130/131 + CallInt 52/53/54 (getAPILevel -> 21),
  GetStringRegion 221/UTFRegion 222 (5th arg from guest sp), void thunks
  return r0=0 (device-grounded: game checks DeleteWeak r0 at 0x267FD0).
  Mocks: DeviceID 0123456789abcdef (HARNESS_DEVID), locale en_US
  (HARNESS_LOCALE). JavaVM table (GetEnv#6/Attach#4). JNI_OnLoad runs by
  default (NOONLOAD=1 skips).
- VERIFIED VIA JEV (batches on file): add-pc raw 1.0; nest-data 0.97; void-r0
  0.76 (Dalvik-0 premise = tunable); BSD-ctype 0.68 (high bytes unproven).
  Tunable assumptions: Dalvik-0, ctype-high-bytes, API21, en_US, device ID.
- MCSM COMPARISON (their .apks, read-only): same architecture independently
  converged (guest pristine in assets/guestlib, Dynarmic host, NDK r27, no
  guest code patches — only ResourcePatchSet for DATA). Differences: they use
  RegisterNatives (1 JNI_OnLoad, 0 Java_*), fastmem (we don't yet), coarse
  gameplay hooks vs our per-import thunks. Their engine boundary is thinner
  (no analytics/payments/social/Lua tax like ours).
- CURRENT WALL (update) REVISED 2026-10-10: the throw is
  `util::JSON::ParseError` ("Unexpected EOF") — PROVEN by a throw-site probe:
  /tmp copy of the .so with `b __cxa_throw`@0x2A6E68 replaced by `svc #0`
  (HALT; originals never patched), halt regs r1(typeinfo)=0x666450 ->
  [vptr 0x66E448, name 0x604950] -> `N4util4JSON10ParseErrorE`. The
  `rcs::Consents::Impl::getConsents` string (@0x609C30, via typeinfo @0x66644C)
  is the NEIGHBORING typeinfo (off-by-4 manual read, machine-corrected) — the
  consents-provider hypothesis is REFUTED as the throw type (though a Consents
  function may still own the JSON-parse call; subagent on static ownership).
  Dynamic pre-throw chain: 0x2A68B4 -> PLT 0x536F0 -> 0x457D64
  (__cxa_allocate_exception) -> 0x2A6DCC -> 0x2AB6B4 -> ... -> 0x2AB738 ->
  0x386954 -> ... -> 0x2A6CB8 -> raiser 0x2A6E4C -> PLT 0x53708/10 ->
  0x457F90 (__cxa_throw). Files read inside update: highscores.lua (empty
  seed, 0-size fread), settings.lua (empty), bi_data.lua (32B), asset
  data/variant/variantconfig.json (1904B, encrypted — all APK *.json start
  with non-UTF8 bytes), fusion.registry (write-only here, never fread).
  Post-throw memchr('}') scans @lr=0xEE4F4 touch the ERROR FORMAT string
  itself ("Input: "{2}" on line {1}"), not the input. New tools: HARNESS_REGS
  (r0-r12/sp/lr dump at halt), HARNESS_MEMDUMP=<lr> (call-time buffer dump;
  needed because worker-thread racing drifts heap addrs between runs).
  POLICY (from pre-compact history): engine decrypts itself under Dynarmic —
  we only deliver bytes; key-origin is fallback-only if content must be
  fabricated (never break crypto, only recon format/which-file).
- WALL FORENSICS 2026-10-10 (HARNESS_PROBE IR emitter: CallHostFunction with
  live r0/r1/lr at any pc; +ProbeDump2 for r2/r3; class via vtable->typeinfo):
  parser @0x2A5368 takes (ctx, {begin,end}-range) + called 4x from wrapper
  0x2A52F8 (lr 0x2A5340): #1 big valid JSON (decrypted asset via
  ZipEntryInputStream, caller 0x2AD968), #2/#3 literal "null" (caller
  0x2A9D34), #4 EMPTY (caller chain 0x1A522C <- 0x1A413C). #4's string is
  [{0,0}] at 0x1A522C entry. Owner: rcs::Configuration (vtable probe).
  Key built at 0x1A40EC: "session_configuration" (21 chars, pool 0x425495).
  Heap key-table maps it -> fusion.registry (backing file). Builder
  0x17DDC8 refs rodata "cloud" (@0x5C8888) with empty short-circuit
  (ldrb [sp+40], beq). No sockets/getc/fgets/read/fread for registry anywhere
  (only 3 freads: lua seeds); registry fopen-rb x2 + .tmp rewrites but never
  read via any thunk. Fresh-install test (registry removed): still throws
  (svc 15322 vs 16700, registry NOT recreated) — throw is NOT caused by
  corrupt registry content. jev_verify 4/5 auto (ParseError-cause 0.98,
  4-calls 0.99, Configuration/cloud origin 1.0, no-I/O 0.94).
  Git checkpoint repo initialized (source-only: host/src, CMakeLists,
  AGENTS.md; tools/thirdparty/work/APK ignored — 3.7GB total).
- TOOLING LESSONS: shell mangles quoted abs paths with spaces — use
  workdir=project + relative paths. Never bulk-delete without backup
  (harness.cpp main() reconstructed once, verified on 3 baselines). No mental
  hex (0x1AECDC/0x1AEDC saga). Docs-first (Regs()[16] bug). Edit discipline:
  read region first, keep oldString minimal, verify after (3 destructive
  edits caught this session). Subagents: all 9 closed; relaunch by sessionID
  after rate-limit death; no live introspection possible.
