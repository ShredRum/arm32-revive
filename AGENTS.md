# AGENTS.md — AngryBirdsRio arm64 port (dynarmic). FULL STATE DUMP.

## HANDOFF (read first after any compression; updated 2026-10-13 day)
- STATUS: boot→RIO LOADING stable (real art, sessions OK flag=1). Menu NOT
  reached. Tree clean, all code+journal committed. Files dirs: work/files19
  active (+ clean_registry.bin reset before EVERY run — registry is one-shot).
- FUNNEL (ranked): (1) login-ladder: initiation BL 0x5CE50 runs clean, needs
  session-linked +32 (link pattern PROVEN); functors never materialize.
  Trigger-gates verdict ARRIVED (§58d-recipes below + older §notes): ordered
  triple tested (loginchain2: holder fix works, r1=1 clean, D5994(r1=2) clean,
  functors still 0). (2) Node/loop path: loop callable directly (returns fresh
  node*, +0x18=7), builders need graph+valid rect (gate bypass proven,
  writer-fault mapped). (3) Manager pass: needs +172/+176 pulse or forge
  (faults on exclusive@0). (4) Unwinder: CLOSED (R7=0). Time/apdrive: DEAD x3.
  Calls sterile. UPDATE-subscription absent.
- FOUR-SUBAGENT WAVE VERDICTS: see §58 (58a lookup / 58b loop invocation /
  58c delivery-impossibility / 58d appender+recipe). Status HYPOTHESES —
  nothing ran live. Convergent picture: producers dead at every level,
  structure fully mapped, delivery proven only synthetically.
- §59 JOURNALED: loginchain2 (holder kills the fault, sterile), nowait1
  (wait-cap REFUTED as pool-killer), plus912 (+912 always-NULL, CLOSED).
  PROBE5 freed for recon targets.
- NEXT ACTION: ONE live recon run (no new code): standard combo + WATCHPC on
  reachable sites, dump live values for forge recipes (§58 synthesis). Then
  fill per W1+W2 recipe (forge outer + populate L1/L2 + BL 0x5F96C), verify
  binary (functors? events? builders? film?). Backstop: D1 device capture
  (QUEUED DEVICE WORK; needs live game + gadget reinstall + calm session).
- DO NOT: drop fill (death@update#1), trust probe-emit/PCTRACE counts
  (translation≠execution — count [probe]#N), hardcode heap addrs (drift),
  run parallel device sessions, bulk-edit without backup.
- DETERMINISM: taskset -c 0, PACEMS=400, 63-call jni_long.txt + JOVR taps
  (early only), NONET+DNSHUSH+empty-DNS, RETSUCC, POKE1187, REALGL film.

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
- NON-NEGOTIABLES (check every plan against these BEFORE acting; user-stated):
  1. The game works OFFLINE on device with ZERO network data. NEVER fabricate
     network success to fix the stall — execute the offline fallback instead.
  2. No run/config that contradicts a settled PROVEN fact without new evidence.
  3. One hypothesis -> one binary-outcome test -> journal verdict. No returns.
  4. No parallel/interfering sessions on device (they wedge the channel and
     crash the game); one calm session at a time.
  5. Commit state (AGENTS + code) before switching topics, so nothing lives
     only in conversation.

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
- CTYPE FIX (2026-10-10, subagent-proven mechanism, self-verified): our
  _tolower/_toupper tables were u32[256], but the game reads u16 tab[c+1]
  (2014 Bionic short tables with EOF-shift). Rebuilt as u16[257] ([0]=0xFFFF,
  [c+1]=case(c)). Old layout read back zeros through the u16 lens. NOTE: this
  did NOT yet move the font wall (same IOException/svc) — the "PVR" key was
  already resolving; lookup miss is elsewhere (0x2B0A3C scan behavior under
  probe with raisable cap HARNESS_PROBE_N, default 8).
- WALL #3 (update, 2026-10-10): io::IOException "Unsupported image file
  format while reading data/fonts/864x480/FONT_BASIC.pvr" (message captured
  with {0} substituted via probe at 0x292118). Mechanism: 0x2B0BD4 returns
  NULL -> `sub r0,r5,#1; cmp #10; bhi` throw-branch at 0x291EC0. 0x2B0BD4
  takes an io::InputStream stack wrapper (vtable @0x66ACF8) whose +28 virtual
  forwards (0x2B07E0) to a heap io::BundleInputStream (vtable @0x66B1A0);
  bundle reads return EOF. Bundle members: [+24]=0x31 [+28]=0x21 (pos>end
  would explain EOF; subagent on layout). Resolver 0x2BD418 zeroes OUT when
  accumulated < needed (0x2BD560-68); its 0x2BD498 path never executes.
  Asset bytes are byte-exact (inflate verified). Format table in heap
  (BMP/TGA/JPG/DDS/PSD/PNG seen) but NO .PVR/.WEBP/.ETC/.PNG found by
  HEXFIND post-mortem (HEXFIND `break`-on-gap bug FIXED -> page-skip; still
  zero — table may be transient/freed or addresses drift). BSS 0x674488 =
  malloc-arena-ish block (writes at 0x2BD9DC/E0 are allocator free-list ops,
  NOT manager registration — earlier theory corrected).
- JNI TALLY (all verified vs NDK jni.h struct order +4 reserved): implemented
  6,15,17,21,22,23,24,25,28,29,30,33,34,35,36,52,53,54,61,62,63,94?,113,114,
  115,116,117,118,119,129,130,131,140,141,142,144,145,146,147,148,149,150,
  151,164,167,168,169,170,171,184,192,221,222,227,228. MISLABELS FIXED (were
  behaviorally OK): 52-54 were CallLong (not Int; now 0L), 140-142 StaticVoid
  (not Boolean), 221 is UTFRegion (ASCII-equivalent), 222 is Critical (check!).
  RULE: never insert between `case 116:` and `case 34:` (114-116 fall through
  into 34-36; broke twice, caught by config baseline).
- GLES SHIM: CreateShader/Program nonzero; Shaderiv/Programiv TRUE; InfoLog
  empty; GetString static ARM/Mali/ES2.0 + ETC1 ext list (HARNESS_GLVENDOR /
  GLRENDERER tunable; PC-only scaffolding, real driver answers on device);
  GetIntegerv era limits (4096/8/16/...); display 800x480 (was 0).
- FILES: /sys + /proc read-only passthrough (cpu topology sizing).
- FLAKE (harness-only, 2/40 runs): main stuck in mutex_unlock trampoline
  (same-pc beats) while worker blocks in file I/O holding the global
  file_lock(mutex_) (probable pipe-read with no writer yet). Device-safe
  (real bionic locks, no global). Watchdog aborts rc=42; rerun proceeds.
  TODO: drop file_lock across blocking ::read/::write if flakes increase.
- PORT LEDGER (do NOT ship): probe infra, fill-{} hack, GL shim+strings,
  /sys/proc passthrough, mock JNI values, seeds. SHIP: loader, real thunks,
  threads, asset bridge, build.
  gl* imports (inventory == binary), zero egl. Signatures from
  /usr/include/GLES2/gl2.h: 41 scalar-void + 2 scalar-ret + 27 with pointer
  args (BufferData/ShaderSource/Gen*/Get*/ReadPixels/TexImage2D/...).
  Highest-risk: ShaderSource (ptr-to-ptrs), multi-OUT Get*InfoLog,
  GetString (RET-PTR — must copy to guest), VertexAttribPointer/DrawElements
  (client-ptr vs VBO-offset duality). Current: all gl* hit the generic
  [stub]->0 tail (dispatch.cpp:1849-1867), so CreateProgram/Shader return 0.
  Host: libGLESv2.so.2.1.0 + NVIDIA 615.71.09, libEGL mesa+nvidia,
  EGL_MESA_platform_surfaceless + KHR_surfaceless in headers (runtime TBD).
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

## 10. Session 2026-10-10: ParseError bypass -> font wall -> update+render GREEN
- PARSE BYPASS (HARNESS_PROBE=1a410c,fill): the {} fill at 0x1A410C (empty
  cloud-config string) lets the JSON parser proceed with defaults. Wall
  BYPASSED, update advanced 16.7K -> 143K -> 586K svc. Fill is harness-only
  scaffolding (does NOT ship). begin_catch NEVER fires on these paths (0 hits)
  — no C++ catch-and-fallback; every throw is fatal spiral (NESTMAX=200000
  proves: blows 8MB stack, genuinely uncaught).
- FONT WALL (io::IOException "Unsupported image file format ... FONT_BASIC.pvr",
  svc~585909 deterministic NOWORKER): 0x2B0A3C image-format lookup returned
  NULL for key ".PVR" though static 20-entry table HAS PVR at index 11.
  - BYPASS FIRST: HARNESS_FIXPVR=1 (IR SetRegister r0=11 at 0x2B0C50) moved past
    the font wall to a new frontier (audio wrapper BAD read) — proved forcing
    the right index suffices (jev_noul 0.93). Real fix superseded it.
  - ROOT CAUSE (PROVEN live): key buffer held "__1" not "PVR". Writer is
    toupper-copy @0x387DE8 (string copy + `tab[c*2+2]` u16 loop). It does TWO
    loads: `ldr r2,[pc+r2]` (=*slot=TABLE) then `ldr r4,[r2]` (reads
    [TABLE]=0xFFFF, our EOF word!) then indexes rodata garbage [0xFFFF+c*2+2].
    I.e. the game DOUBLE-DEREFS toupper/tolower slots (Bionic exports POINTERS;
    slot->var->table), but we patched slots with the table DIRECTLY (single
    level). Live proof: probe at 0x387E6C showed r2=0x10000308 (table, not
    &var), r4=0x0000FFFF, chars P/V/R correct, [r4]=rodata typeinfo bytes.
  - FIX: var_lower/var_upper 4B cells holding lower_va/upper_va; slots patched
    with &var (same pattern as _ctype_/guard). Isolated lookup HARNESS_RAW=
    2b0a3c now returns 11; full run blasts past fonts into SPLASHES/TEXTS
    assets. NOTE: the earlier u16-table rebuild was NECESSARY but not
    sufficient (tables were correct; routing was wrong).
  - MEMCMP RED HERRING: host glibc memcmp returns sign-only (±1), NOT byte
    difference ([memselftest] prints `1 0 1`). Wild memcmp3 magnitudes
    (58523644 etc.) are glibc internals — game uses sign/zero only, our
    forward is correct. Do NOT chase memcmp magnitudes again.
  - SSO ASYMMETRY (PROVEN via HARNESS_RAW=69cdc): in-game compare treats r3 as
    `const char*`; passing an SSO buffer misaligns (PVR vs 06 50 56 -> 74).
    Live path passes raw static entries — fine. No action.
  - Loop anatomy 0x2B0A3C (for the next reader): returns INDEX (0=not found,
    r7 distinguishes found/exhausted); per-entry multi-alias inner walk;
    length prefilter; full compare via 0x69CDC; caller 0x2B0BD4 switches on
    index through jump table (0x2B0BC0). rfind('.')@0x17EE94, substr@0x6A1F4.
- REGISTRY SAGA (after font wall, update svc 650K ParseError):
  - Decrypted registry plaintext: {"ads":{"installTracker":null},
    "pf::LocalNotifications::sm_pendingNotifications":{"EID_INACTIVITY_
    NOTIFICATION_3":0x01}} — byte 0x01 is not JSON.
  - Bridge EXONERATED: [regio] fwrite/fread dumps + file bytes all 128/128
    identical (roundtrip byte-exact). Game serializes 0x01 itself.
  - Map node dump: pending value = int32 1. Writer emits int 7 as "7" (valid!)
    but int... 0-case emits raw 0x01. Mechanism: notifyAfter mock returned 0
    -> game stores bool-true fallback -> writer emits bool as raw byte.
    PROOF: notifyAfter->7 gives ":7}}" (parses); ->1 gives ":1}}".
  - FIX: notifyAfter mock returns 1 (first notification id, device-plausible;
    real Java returns nonzero id -> decimal text -> parses). Device works
    because real Java never returns 0. JNI int-mock policy grows: API21,
    800x480, notifyAfter=1, else 0.
  - FRESH-INSTALL RULE: no registry file -> io::FileException (r1=0x65ED5C)
    at svc~15K (game cannot bootstrap missing registry). SEED empty
    work/files/fusion.registry (touch 0B) -> long path. Poisoned file (with
    0x01 entry) kills at 15K — delete/reseed when iterating. work/files seeds
    are harness-only scaffolding.
- MILESTONE 2026-10-10: nativeUpdate RETURNS CLEAN (r0=1, svc~643730, no
  throw/spiral) + nativeRender RETURNS CLEAN. Real GL traffic: 20x
  TexImage2D + 20x TexSubImage2D (texture uploads!), 12x ShaderSource,
  GetString/GetIntegerv/attrib+uniform locations. Shaders compile, textures
  upload (fake-success shim still — real GLES backend is NEXT).
- WALL #4 PARKED (2nd serial update): re-entering update (with or without
  render between) dies svc~653206: BAD reads from 0x30303038+ (ASCII "000...")
  in block-copy @0x264AF8 (ldm/stm list/object copy; reported pc 0x264B10
  lags — real fault is later ldm from bogus source ptr). Needs threaded
  model (update/render/audio interleaved) or audio/input state; revisit after
  audio+input milestones. Single update+render is the validated unit.
- DETERMINISM: heap addresses stable across NOWORKER runs (same #8 buffer
  addrs twice) — watchpoints reusable. Registry FILE STATE selects path
  (missing->15K FileException; poisoned->15K ParseError; empty/fresh->650K).
  Always note file state when comparing svc counts.
- DIAG TOOLS ADDED (harness-only, do NOT ship): HARNESS_NOWORKER (skip
  pthread_create, deterministic); HARNESS_NESTMAX (default 512); HARNESS_RAW
  (+RAWDOT/RAW2: isolated guest micro-calls, inits run first); ProbeDump3
  (r4/r5/r6 + [r4] table peek); [probe-emit] A/T log; memcmp3 len-3 compare
  log (HOT — fires constantly, silence before perf work); [regio] registry
  fread/fwrite dumps (cap 60); [memselftest] (glibc sign proof);
  HARNESS_WP/WPA (exact-addr only; bulk writes bypass -> use ScanWP);
  HEXDUMP len is HEX; HEXFIND heap needle (5 contexts, page-skip fixed).
- JEV: noul batch (force-11 0.93 likely; audio-same-cause 0.28; GLES-needed
  0.29) — audio wall and GLES gating stayed uncertain, correctly (both were
  downstream of the real toupper bug).
- NEXT (revised): 1) real GLES backend (surfaceless EGL, real uploads; shim
  fns enumerated in dispatch ~glGen*/glTex*/glShaderSource...), 2) audio
  null-sink (nativeMixData needs boot context), 3) input, 4) wall-#4 revisit
  (threads), 5) RegisterNatives cleanup, 6) Android shell + packaging.

## 11. Session 2026-10-10 (part 2): wall #4 DOWN, real GLES, FIRST FRAME
- WALL #4 ROOT CAUSE (PROVEN): our `__aeabi_memset4/8` thunk had C arg order
  (dest,c,n) but ARM EABI is (dest,n,c). Game's `memset(obj+8,48,255)` meant
  48B of 0xFF; we wrote 255B of 0x30, poisoning Technique/Pass vector headers
  with 0x30303030 ("0000"). Two self-inflicted wounds in one: (a) order swap,
  (b) a broken `compare(0,13,...)`-vs-14-chars gate that silently disabled the
  first fix attempt (std::string::compare with unequal lengths != 0).
  RULE: EABI helpers (`__aeabi_memset/memclr/memcpy/memmove`) need per-name
  order checks, never one shared shape. Verdict on the "layout disease"
  detour (MALLOCPAD etc.): wrong theory, allocator is fine, all TEMP pad code
  stays off by default.
- Subagent (static, read-only) delivered the ownership map that made this
  solvable: 828B object = `gr::gles2::GL_Pass` (clone 0x264C74/copy 0x264AF8),
  vector owner = `gr::gles2::GL_Technique` (+20), readers = vector<16B>
  push/grow (NOT map lookups), writer candidate = memset at 0x25E798.
  Lesson: narrow read-only static tasks parallelize well with live forensics.
- REAL GLES BACKEND (host/src/guest/realgl.{h,cpp}, HARNESS_REALGL=1): EGL
  (X-window surface when available, pbuffer fallback; transient window only,
  zero persistent system changes) + full forwarding: shaders (real compile,
  \\x01 header separators sanitized to \\n — 2014 Mali/Adreno accepted them,
  strict compilers don't; all 6 pairs compile+link on NVIDIA), programs,
  attribs/uniforms (real locations), RGBA4444/8888 texture uploads with real
  data (game decodes .pvr itself; no PVRTC reaches the driver), state,
  VBO-offset-vs-client-pointer duality via bound-buffer tracking.
  gltest tool validates shaders offline; eglmin is the minimal EGL path probe.
- FIRST FRAME (PROVEN): 8 serial updates + render, all clean, Rovio logo
  splash pixel-visible (work/shot.ppm + live X window). Draw path: prog 9,
  NDC verts, identity MVP, texture 805x439, all err=0.
- BLACK-FRAME SAGA (all harness bugs, not game): (1) RGB ReadPixels from an
  alpha-bearing config = GL_INVALID_OPERATION -> zeros (read RGBA!); every
  black screenshot before this fix was the read bug. (2) Live window stayed
  black until eglSwapBuffers after readback (front/back). HARNESS_SCREENSHOT=1
  + HARNESS_SHOW=N (keep window open for viewing).
- DIAG ADDED (harness-only): HARNESS_JLAST (last-call r2/r3 override, for
  mixdata), [glsrc]/[gltex] signature logs, work/shaders/*.glsl capture,
  draw-state dump (prog/tex/vp/blend/scissor/attrib + MVP + texsize),
  upload checksums, HARNESS_MALLOCPAD (layout test, default 0),
  HARNESS_NOBLEND/HARNESS_SHOTCLEAR/HARNESS_SHOTSWAP/HARNESS_EGLD/HARNESS_EGLX
  (readback experiments, keep for future GPU debugging).
- NEXT: audio (mixdata needs the AudioOutput J handle from NewObjectV log +
  null-sink), input, menu frames past splash, RegisterNatives, packaging.

## 12. Session 2026-10-10 (part 3): input proven, LOADING gate, audio live
- INPUT (PROVEN): nativeInput(IFFI) @0xc23e4 takes (action,x,y,?) — down/up
  at center return clean. 20 updates no-tap = Rovio splash; 20 with tap = Fox
  splash. Tap ADVANCES boot. HARNESS_JOVR="idx=r2,r3,s0..” per-call overrides
  (f-prefix = float bits, A = byte array) — general input/event injection.
- BOOT PATH: Rovio -> Fox -> Angry Birds Rio LOADING (Blu+Red, Rio landscape,
  real pixels, svc~1.8M, 48 updates + taps, all clean). Live window + shots.
- LOADING GATE (stuck, idle redraws identical from ~upd40): late updates do
  pure redraw (27 lines: memcmp3 + 4 draws, zero JNI). Main-thread asset queue
  drains (422 in upd16, then trickle). Ruled OUT: network (zero socket calls),
  worker (50-update live-worker run also LOADING), more updates (48 enough),
  taps during loading (6x, no effect). RANKED hypotheses: (1) CONSENTS gate
  (rcs::Consents NOT_ANSWERED with no UI to answer — the original wall-#1
  owner; needs answer mock), (2) AUDIO clock (mixdata never in loop; see
  below), (3) Lua timer/event.
- AUDIO LIVE (milestone): AudioOutput.<init>(JIIII)V created by game;
  NewObjectV varargs are at the a3 va_list pointer (NOT sp — first log was
  garbage from misread). J recorded in Runtime (audio_j_lo/hi);
  HARNESS_JLAST/JOVR "J" expands it. mixdata(J[BI) with real J + array RETURNS
  CLEAN (GetByteArrayElements + Release, r0=0) — audio engine object EXISTS.
  Sustained loop FAILED on 2nd call: game passes array=0 (reads it from a Java
  object FIELD via GetObjectField on our mock -> 0). Real audio milestone =
  Java object-field state emulation. PARKED here.
- DIAG: HARNESS_JOVR, HARNESS_JLAST "J", [audio] ctor log, AudioJLo/Hi.
- NEXT: (1) consents-answer mock (registry/state archaeology), (2) object
  fields for audio, (3) menu + gameplay, (4) packaging.

## 13. Session 2026-10-10 (part 4): MENU HUNT — splash path mapped, LOADING gate
- INPUT MAP: nativeInput(IFFI)@0xc23e4 + HARNESS_JOVR per-call overrides
  (generate index lists with python — hand-counting caused 2 index bugs).
  Proven: 20upd no-tap=Rovio, 20upd tap=Fox. Splash screens wait for input.
- BOOT PATH: Rovio -> Fox -> Rio LOADING (Blu+Red, landscape, 4 draws,
  svc~1.8M). Live X window + PPM shots; HARNESS_SHOW=N keeps window open.
- LOADING GATE (open): idle updates = identical redraws (zero JNI). Ruled
  out: network (0 socket calls), worker (50-upd live run stuck too), count
  (48), loading-phase taps (6x). Static gate analysis backgrounded.
- AUDIO (live, parked): mixdata with recorded ctor J returns clean, 5x loop
  OK. The "array=0" failure was my JOVR index bug, NOT field mocks
  (retracted). Mixing so far doesn't unblock loading.
- SHOT/GL DIAG: RGBA reads (RGB=INVALID_OPERATION on alpha configs),
  swap-after-read, draw-state/MVP/texsize dumps, upload checksums, glScissor.
- TIMER THEORY DEAD (2026-10-10): HARNESS_TIMEWARP=N (shifts clock_gettime/
  gettimeofday/time) +3600s at loading gate -> still LOADING. Gate is
  event/state, not time. TimeWarp helper in dispatch.cpp (file scope, avoids
  goto-initializer issue).

## 14. Session 2026-10-10 (part 5): MENU HUNT systematized (no more blind pokes)
- BOOT PATH (firm): Rovio -> Fox -> BlueSky -> Rio LOADING. Splashes advance
  on TAP (nativeInput proved: 20upd tap=Fox vs no-tap=Rovio) and on FRAMES
  (render-spam reached Blue Sky). Taps at 7 positions no-op at LOADING.
  Renders interleaved don't unlock (c22f0 ~empty, c22f8 tried, frameClear
  c1fd0 tried, resume c23dc tried — all clean, none unlock).
- LOADING GATE (still open): idle updates byte-identical (md5 x6), zero JNI,
  4 draws, 858 assets all OK. Subagent statics: (a) app+1187 (reader 0xBE714)
  is QUIT flag (poke derails to quitRequested — HARMFUL, keep off); (b) draw
  func 0x263300 r4=gr::gles2::ShadowingSurfaceData, state +180 stays 3 (switch
  1..6), +188 vs 4, +184 flags; NO virtual method writes 180/188 (transition
  external); draw sites 0x2637A0 (mode/count from +92/+96: first=0,count=12,
  static) + GL_Primitive 0x2646d0; no BindTexture/UseProgram in paths.
- POKE180 (3->4 at FuncA entry, live r0): FIRES, flips 4 instances, produces
  TORN frame (wrong pass composited) — proves render control, wrong direction.
  Lesson: IR probe/poke hooks fire ONLY at block starts (0x263338 mid-block
  never emitted; 0x263300 entry works). Leaf state setters 0x12CECC/0x12DDF4
  NEVER fire live (0 hits) — transition truly absent.
- SHADOW DATA (static): counts +92=0/+96=12 frozen; vptr 0x669710 live.
- EVENT SYSTEM: advance path (ex-poke) references "quitRequested" + BSS obj
  0x672DF0; no catalogLoaded strings in heap (never posted without poke).
- WORKER RACES (systematic): identical inputs -> hang / NULL-jump / clean.
  Hang protocol mapped: main creates pipe in UUID/install flow, polls, gets
  ONE 0x00 wake, starves; all 3 workers exit; sometimes a worker eats main's
  wake (cross-thread pipe race). UUID.toString mocked real (didn't help).
  localtime_r/gmtime_r implemented (didn't help). __system_property_get serves
  net.dns1/2 (no sockets ever called — DNS dead end). strtok_state made
  thread_local (real hole). pthread_cond_wait capped 5s like timedwait
  (HARNESS_NOCAPWAIT=1 disables). Heartbeat pc/lr proven TORN (do not trust
  for location; mem 0xF0000348=clock_gettime + lr 0x38C988=ioctl-ret was a
  false spin theory — real hang is poll starvation).
- MOCK AUDIT (device-plausible values): hasSystemFeature (35 features logged;
  touch/wifi/telephony/location=1, sensors/camera/mic/BT/NFC=0),
  networkType=NONE (HARNESS_NETTYPE ovr), Utils.getViewWidth/Height=800/480,
  getPPI=240, density/config groups=1. JNI V/A args live behind va_list/jvalue
  pointers (fixed hasSystemFeaturemede arg read). None unlocked menu yet.
- HARNESS: HARNESS_LOOP still TODO (hand-rolled 100-call lists keep causing
  index bugs — RULE: generate with python, twice bitten). HARNESS_JOVR f-float
  + A-array + J-handle expansion; HARNESS_JLAST; HARNESS_SHOW (view window);
  HARNESS_EGLD/EGLX device selects; gltest matrix (ES2/ES3/FBO/blit/swap);
  eglmin minimal probe.
- ACTIVE SUBAGENTS (background): (1) shadow-writer 0x0E9198 caller chain ×2
  levels; (2) render-thread role (c22f0 vs c22f8, update waits for render ack?).
- LOADING GATE intel: decrypted Lua string consts persist in heap (menu init
  registry readable: initializeGameMenus creates all pages incl. LoadingPage/
  MainMenuPage/EpisodeSelection; no code bodies, only consts). Per-update "ADD"
  memcmp = volume-key input polling (VOLUME_UP/ADD rodata @0x5C0D59), harmless.
  Idle updates byte-identical (md5-proven, 6 in a row) = pure flag wait.
- TMP-ARCHIVE (work/tmp-archive/, git-ignored): rescued from /tmp before a
  shutdown — eng_idx11.so + eng_throwprobe.so (patched .so experiment copies),
  registry_poisoned_128B.bin + regbak/ (0x01-wall forensics), milestone PNGs
  (shot7 first frame, tap3/tap4 LOADING, menu5, notap20).

## 15. Session 2026-10-10 (part 6): subagent verdicts, ETC1 stall, sscanf, cloud
- SUBAGENT render-thread (PROVEN bytes): nativeRender @0xC22F0 = `mov r0,#1`
  (const); nativeRenderThread @0xC22F8 = single ldrb from BSS 0x672E68 (getter,
  no writes). nativeUpdate polls 3 gates once and bails to 0xC2244 (NO loop):
  *0x672E98!=0, vcall+148==1 (live: returns 1 from upd#3 on), byte[singleton+
  1187]==1 (=quit flag reader 0xBE714). Gate#3 without poke = skip quit-check
  + draw loading (correct idle behavior). No render-side ack flag exists —
  concurrency per se is NOT the fix; the producer of +148/+1187 state never
  runs in harness. (Note: 0x672E9C from old notes is a typo for 0x672EA0.)
- SUBAGENT shadow-writer (PROVEN bytes): 0x0E9198 stores strtol(elem+28) into
  ShadowingSurfaceData+188 iff map-find("unseenAds") hits and value>=1; callers
  0x0E83D4 (orphan, no static caller) and 0x0EC6DC (inside rcs::SessionImpl
  vtable slot-0 loop 0x0EC188 — network session callback). UNREACHABLE without
  network/session; +188 writer silent live (0 probe hits) — transition absent.
  +188 is an ads-counter, NOT the loading gate. Dead end for menu.
- ETC1 STALL (PROVEN): asset queue drains through TUTORIALS_PLATES1 then stops:
  PLATES2/3.pvr are ETC1 (format byte 6) vs RGBA elsewhere; game never uploads
  them (TexImage2D missing for exactly those two). glCompressedTexImage2D/Sub
  were unhandled -> now REALLY forwarded (realgl.cpp, llvmpipe accepts ETC1;
  NVIDIA errors -> game software-decodes). ETC1 decode is a WORKER job:
  NOWORKER run stalls at first ETC1 (upd32). etc1a/b logs.
- SSCANF (root-caused worker instability): worker parses URLs via sscanf
  (was a ->0 stub). Implemented manual C99 sscanf in dispatch.cpp (ws-skip,
  widths, suppression, hh/h/l/ll, d i u o x X p f e g s c [ n %; guest long =
  32-bit; %n no-count; unsupported conversion = LOUD halt). Result: worker
  runs clean 3/3 (was hang/crash/clean nondeterminism). TEMP-DIAG [sscanf]
  input log (30-line cap) kept for URL forensics.
- CLOUD (PROVEN live): worker parses https://cloud.rovio.com/session/1/apps/
  AngryBirdsRioFull/{time,sessions} + apdrive/.../assets?name=variantconfig.
  json (URL dissect formats %15[^:]:%[^...] + IP-literal check). Zero socket
  calls reached; cloud1.log DIED at 3 updates on LOUD fopen("/etc/hosts") —
  the bundled HTTP/DNS stack reads it. FIX: /etc/hosts + /etc/resolv.conf
  read-only passthrough (same pattern as /sys//proc). Socket family still
  generic stub ->0 (socket()=0 is fd-0 alias hazard; connect()=0 success-lie —
  watch when DNS completes). hosts1.log tests the fix.
- MOCKS mined from worker flow (all device-plausible, none unlocked menu):
  hasSystemFeature table, networkType NONE (HARNESS_NETTYPE), 800x480/PPI240,
  UUID.randomUUID real, localtime_r/gmtime_r real, GetObjectClass #31 tracked,
  net.dns1/2 via __system_property_get. Taps (7 positions) no-op at LOADING;
  renders/frameClear/resume interleaves no-op; TIMEWARP +3600s no-op.

## 16. Session 2026-10-10 (part 7): socket bridge, io_mutex_ deadlock fix
- SOCKET BRIDGE (host/src/guest/dispatch.cpp, works on dev+device): game has
  its own DNS-over-UDP resolver (raw socket/connect/send/recvfrom, no
  getaddrinfo import). Old stub ->0 aliased stdin + faked success (4x retry
  then null-call pc=0). Now real host sockets: guest fds via fd_map_,
  per-thread errno via ErrnoSlot, sockets O_NONBLOCK, I/O poll-capped 5s,
  TCP connect capped 10s (nonblocking+POLLOUT+SO_ERROR). HARNESS_NONET=1 ->
  socket()=-1/ENETUNREACH (fast deterministic offline). Bionic/Linux errno +
  sockaddr layouts match. fcntl/ioctl still stub->0; sendmsg/recvmsg LOUD.
  Self-review fixed 3 bugs (getsockopt len check, listen backlog as pointer,
  set/getsockopt read/write direction). TEMP-DIAG: [netsock] calls + inbound
  hex (40-cap) + error lines; opendir path log (15-cap).
- REAL DNS PROVEN (iofixA): socket->gfd132, connect->0, 33B query out,
  4x recvfrom. Then main died @0x39A7F8 wild-index into table 0x66BCA8
  (after DeviceID.getUniqueId) — unknown if DNS-reply-driven; inbound hex
  log will show next time.
- IO_MUTEX FIX (the worker-flake breakthrough): file bridge held mutex_
  across ALL syscalls; a worker parked in blocking ::read (pipe, no writer)
  froze every thread (main "stuck in fwrite/unlock", beats TORN at
  clock_gettime slot — heartbeat location UNTRUSTWORTHY, again). Fix: outer
  guard removed; leaf io_mutex_ with brief holds at each fd/file/dir map
  touch (helpers host_fd_of/alloc_guest_fd/erase_guest_fd + guarded lambdas);
  ALL blocking I/O runs unlocked. Baseline unchanged (54/54, svc=948).
  Result: 2/2 worker runs progressed (no 300s виса that round).
- NONET LONG (iofixB, 6upd): main 100% clean through 6 updates (r0=1);
  worker died in throw-spiral (BAD read 0x464C458B [r2+12] @0x5C5B0 during
  opendir/readdir — ELF-magic-as-pointer, same class as §5 stage slots:
  worker reads a slot main hasn't written = race or orphan slot).
- HANG SHAPE (nonetlong1, STUCK@300s): 7 updates, pipe wake delivered+read,
  then poll-ret 0s, svc FROZEN (no SVCs at all — not a poll loop; heartbeat
  pc=TORN garbage). Main is inside ONE blocking host call (prime suspect:
  uncapped ::read on pipe, or join). Worker wrote ONE 0x00 byte then went
  silent (exited? crashed? — thread-done lines exist, check them). Missing
  2nd message = the hunt. gdb-attach plan queued (gdb2.log running).
- ROVIO STILL FINE (2026-10-10, rovio_check.png): NOWORKER 8upd renders the
  logo pixel-perfect — no render regression; worker runs just die earlier
  (3-7upd, DNS phase) before splashes + end-of-run screenshot.

## 17. Session 2026-10-10 (part 8): pipe solved, cloud dead, throw theory
- SUBAGENT (read-only statics, delivered): pipe owner +36=read/+40=write/
  +44=pending (LDREX/STREX inc/drain), 1B per wake, reader 0x28E328 coalesces
  (poll 1000ms + read min(pending,256)); senders = job-completion (strb#1
  [job+4]+dmb, dequeue, run next), enqueue, queue-drain; creator 0x28D928
  (pipe() + store fds). Subsystem = NDK TaskDispatcher ("Dispatcher
  disposal"/"Job"/"Got exception" rodata; thread entry 0x189338 = functor
  thread via helper 0x188060 = __thread_struct/__thread_local_data; pool
  0x38C048 same helper). Crash-walk func = 0x1A3378 (ordered-map/SSO-key
  lookup + memcmp; sole caller 0x1A3314 in 0x1A32E4; writer = head-insert
  0x1A522C in Configuration chain; NO thread BL-edge into the map — writer
  is main-thread; [r5]=0x5C005E3E likely wrong-object, not thread race).
  0x45A778 = Thumb fragment (not ARM dispatcher body).
- CLOUD DEAD (host recon): cloud.rovio.com = CloudFront, alive at TCP/TLS
  but API gone: session/time=403, variantconfig=401, sessions POST=4xx error
  page. Real users today take the FAILURE path too — so the offline/failure
  path (not success) must lead to menu.
- THROW THEORY (leads): failure path throws inside a dispatcher job (job
  exception); device unwinder runs the catch ("Got exception") + fallback;
  OUR unwinder through translated code is broken (§6/§10: begin_catch never
  fires, every throw = fatal 513-spiral) -> worker halts HOLDING the queue
  mutex -> main blocks in mutex_lock (frozen svc, silent log, TORN beats —
  all consistent; join never called, sleeps capped, read capped).
  NONET refined to airplane-faithful (socket() SUCCEEDS, connect()=ENETUNREACH
  instantly; old socket()-fails sent game down never-tested path -> BAD
  @0x1A343C). Read-cap added (FIFO/sock reads poll 5s -> EAGAIN; was the
  STUCK@300s suspect, now exonerated — hang persists with frozen svc).
- NEXT: (1) gdb-catch a hang (watcher script /tmp/opencode/watchhang.sh:
  log-silence -> auto gdb bt) to CONFIRM main-in-mutex vs guest-spin;
  (2) throw-type forensics (patched-/tmp-copy __cxa_throw probe, ParseError
  playbook) on the worker job; (3) bypass/fallback to menu.

## 18. Session 2026-10-10 (part 9): MCSM reference autopsy (read-only)
- SOURCE: /home/shredrum/Рабочий стол/Lifeline_port (their project dir) +
  shipped `lib/arm64-v8a/libmain.so` (3.5MB, arm64, NDK r27c, RTTI intact)
  extracted from Minecraft.Story.Mode .apks base.apk to /tmp (source files
  never touched). Guest libs pristine in assets/guestlib (GameEngine 17MB +
  SDL2 + fmod + 9KB main shim).
- THEIR HOST (proven via strings/nm/readelf): single export JNI_OnLoad;
  NEEDED GLESv2/GLESv1_CM/EGL/OpenSLES/log/android/m/z/dl/libc; AAssetManager
  guest loading; ANativeWindow real surface; RegisterNatives (Java_* names
  SDLActivity_/TelltaleActivity_/fmod present as STRINGS, not exports);
  DynarmicHost::LoadModulesFromMemory (multi-module vector<GuestModuleImage>);
  ThunkManager::RegisterLibcThunks (~286 lambdas) + RegisterGLThunks (~104)
  — SAME granularity as our ~340 imports (convergent, not thinner);
  GuestThreadState/Mutex/Cond/Semaphore; Hooks::Execute + ResourcePatchSet::
  Apply (coarse gameplay hooks + DATA-only patches).
- KEY ADOPTIONS: (1) FASTMEM ("POSIX SigHandler", "Segfault happened within
  JITted code") — perf only, deferred for us (2D game fine on callbacks);
  (2) LIBUNWIND EH (_Unwind_RaiseException/_Unwind_VRS_Set/_Unwind_Resume/
  __cxa_type_match/scan_eh_tab/__gnu_Unwind_Find_exidx/_Unwind_DeleteException)
  — they unwind GUEST exceptions; this is our #1 blocker class
  (HttpRequestException spiral). Our pieces ALL present: exidx parsed
  (FindExidx), __gxx_personality_v0 in-module @0x458859 (Thumb),
  __cxa_throw @0x457F91 (Thumb) / allocate/begin_catch in-module, Dynarmic
  Regs for VRS. _Unwind_RaiseException itself is neither imported nor
  defined (host must provide it — same as us). PLAN: custom _Unwind_Context
  with VRS over Dynarmic regs + in-module personality via hijack +
  phase-2 pad resume (set regs + jump). Replaces ALL per-throw bypasses
  (fills, catch emulation) — STOP building bypasses, build unwinding.
- NOT REDUNDANT (their engine is SDL-thin, ours pays rcs-fusion tax):
  sockets/DNS/inet (theirs: zero socket strings — no cloud), registry/seeds/
  Lua/consents mocks, JNI mock zoo (theirs calls REAL SDLActivity Java;
  ours needs harness scaffolding until device). 63 Java_* -> RegisterNatives
  at packaging (already planned). No dlopen emulation needed (theirs: none).
- Ahnentafel: patched-copy forensics CONTAMINATED (2B patch in __cxa_throw
  -> 0/9 anomalies vs 3/3 unpatched — engine self-reads; use IR probes,
  never byte patches). Throw probe PROBE2=457f90 prints type cleanly
  (net::HttpRequestException x2/run, lr=0x28AE70 raiser tail in 0x28A3DC;
  callers assume success-flag, no error path — fallback is dispatcher-catch
  only). inet_* ABI bugs fixed via file-scope InetThunk (a0-vs-a1,
  ntoa-by-value, ntop order; stray-brace saga: missing } shifted nesting).

## 19. Session 2026-10-10 (part 10): TLS wall mapped, redirect chains

## 20. Session 2026-10-10 (part 11): fake origin, surface gate, unwinder plan
- FAKE ORIGIN (works, insufficient): hosts-hack honored (TCP straight to
  127.0.0.2), portmap 443->8443, python TLS server (self-signed
  CN=cloud.rovio.com). Client talks (ClientHello!) but aborts every
  ServerHello with alert 47 (python AND CloudFront alike). Client offers
  modern ciphers (CHACHA/GCM) yet rejects hellos: version OR curve OR
  extensions (subagent on the record-layer check). TLS 1.0 downgrade offer
  -> server silent (min 1.2+). Cipher-overlap proof vs CF (alert 40).
  ONLINE PATH DEAD (crypto + dead API 401/403).
- BLOCKING SOCKETS (real bug fixed): O_NONBLOCK caused short reads (5B of
  115B ServerHello) -> alert. Now blocking + SO_RCVTIMEO/SNDTIMEO; poll
  caps off for socks. getpeername/getsockname real. fcntl real.
- LOCAL variantconfig.json (1904B) IS read + parsed (valid JSON #1) during
  boot — data present. Missing piece = cloud-JOB COMPLETION (session token
  -> apdrive), not content. Emulated failures (RETFAIL) run clean but don't
  unlock (failure branches yield no data).
- SURFACE GATE (mechanism proven): ONE ShadowingSurfaceData ever constructed
  (state 3, loading); array has 1 descriptor; caller fork ([r10+164]==0 -> 1
  else 3) is const-2 by init (hardcoded). No menu surface requested: gate is
  UPSTREAM (config/event), not a transition (WPA: +180 write-once ever).
- REDIRECT (works mechanically): Http->0x28E7C4->length_error rethrow;
  chains [28e7c4,189ef0]/[28e7c4,2848c4] all rethrow (translate pads, no
  handlers). 117 pads mapped; all live in functions that also throw.
  RETFAIL (synthetic failing return via live frame: r11 stale, stack-scan
  finds saved-lr, flag=0, resume) works end-to-end, no spirals, but failure
  branches don't unlock menu.
- TTMatch (typed-catch finder): GCC LSDA decoder validated on NDK test .o
  (filter->ttype->reloc); 988 generic entries + 1559 (fn,pad,filter,slot);
  live resolve (onehop + pcrel-indirect twohop) + live base-chain walk.
  Result: catches filter a BASE (0x65E958), never the derived type; NO
  dispatcher-module typed catch for it; on-path frames have no typed
  catches (compact catch-alls only). StackBT (live return-addr scan) gives
  the propagation chain (raiser<-0x28B140<-0x19F9C4<-0x2AE8BC<-pool...).
- DECISION: offline-first (NONET) + unwinder (host phase-1: stackbt frames
  + LSDA filters + live ti match + opcode-sp + pad resume via redirect
  machinery). r0=obj proven accepted; r1=filter (not ti!).
- SUBAGENT TLS verdict (independent): handshake/record validator
  @0x49D33C (Thumb): minor-version pair accepts 1|2, rejects 3 -> fatal 47;
  sender @0x49D3F6 (r1=2); error blocks reference s3_srvr.c. Confirms 1.0-cap.
- NEGATIVES (closed): NETTYPE=WIFI (only 2nd DNS queried, no gate change);
  NETTYPE="" (requests still attempted, LOADING); chain pad 0x38A270 in
  0x38A0E8 (on-path, rethrows like the rest); thread entries 0x38C048/
  0x189338 have NO pads (uncaught = thread death, by design).
- STACKBT chain at Http throw (live): raiser <-0x28B140<-0x19F9C4<-
  0x2AE8BC<-0x38A0E8(pool)<-0x2ABF40 (+ runtime frames). Only 0x2AE8BC has a
  typed entry (no match); single on-path begin_catch pad (0x38A270) rethrows.
- TTMATCH base = 0x65E958 (live), catches filter base-never-derived; no
  dispatcher typed catch; compact entries hold the rest (catch-all shape).

## 21. Session 2026-10-11 (part 12): phone revelations, TLS 1.0 origin
- USER DEVICE (decisive): same build on old phone WITH internet reaches full
  menu (loading animates, menu playable); in AIRPLANE MODE with old cache it
  ALSO reaches menu. So an offline-from-cache path EXISTS. Our harness wipes
  fusion.registry every run (= forced fresh-install = forced cloud path).
  NO network-state queries exist in our logs (no ConnectivityManager/
  NetworkInfo/WifiManager) — gating is via cached state, not Java APIs.
- STATUS HONESTY: "LOADING hangs due to network" is INFERRED, not PROVEN.
  Proven: workers die on throw, config never completes, no menu surface
  requested. The cloud-success->menu link is exactly what TLS-origin tests.
- RAISER ABI (live, PROBE3+RaiserDump): 0x28A3DC(r0=reqstruct, r1=HttpRequest
  ti 0x660CC0, r2=2, r3=aux) lr=0x28B160 both calls. r0 struct: +0x0C "POST",
  +0x4C request JSON body ({"access":{"clientId":"AngryBirdsRioFull",...),
  +0x58 URL. Endpoints seen: .../time, .../sessions (apdrive later).
- IOException ti 0x65DD5C, base 0x65E958 (SAME base as Http) — catches filter
  the common base. ECONNREFUSED still throws (no error-code path); fresh-dir
  runs without seeds throw IOException (contamination lesson: always seed +
  always keep PROBE=1a410c,fill or boot dies at svc~17k).
- EXTAB correction: extab w0 bit31 SET = compact (inline), CLEAR = generic
  personality+LSDA. ttslots (988 gxx entries) stands; earlier "5831 pers"
  histogram was misparse garbage (prel31 of compact descriptors).
- WRITE-PATH REWRITE BUG (real, fixed): printf claimed 0301/0301 but code
  wrote 0303/0303 (0x03 not 0x01). Client had been offering 0303 all along.
- TLS 1.0 ORIGIN (hand-rolled, /tmp/opencode/fakeserv/tls10serv.py): RSA +
  3DES/AES-CBC-SHA, PRF/MAC/Finished correct — LOOPBACK-GREEN (strict python
  client completes handshake + HTTP). Server flight thereby RFC-clean.
- GAME vs origin: ClientHello 0301 + 27 ciphers + ext(SNI/RI-float/EMS/
  sigalgs/GREASE-3374/ALPN/ecpts/groups = BoringSSL-style hello). Answers
  0301+0x0A / 0x2F / SH-only ALL get fatal alert 47 (0x022f), reason(247)
  (same as vs CloudFront). Cipher pick is NOT the (only) issue.
- SUBAGENT alert-47 pinpoint (PROVEN): sender 0x49CCFC; reason-247 site
  0x4AF8DE in ssl3_get_server_hello (0x4AF4AC): CBS-read cipher -> lookup
  0x4A33B0 -> NULL -> beq 0x4AF646 -> ERR(16,247)=0x100000F7 -> alert 47.
  Prior 0x49D33C "version 1|2 vs 3" theory REFUTED (state dispatch, alert 50).
  OPEN: why lookup NULLs offered ciphers (table subset? [sp+32] misland?).
- NEXT: cipher sweep (0x35 running) -> ECDHE_RSA (0xC013, offered) with
  ServerKeyExchange+ECDH+RSA-sign via cryptography lib if RSA-kx all NULL.

## 22. Session 2026-10-11 (part 13): phone dump, graceful catch proven
- SAME BUILD (proven): aapt says our APK versionName=2.6.13 versionCode=2676;
  phone reports 2.6.13; md5 of both libAngryBirdsRio.so identical. Phone
  evidence fully applies. ("2676" in filename = versionCode.)
- PHONE LOGCAT (online): RovioAccount login FAILS 403 (CloudFront HTML) yet
  menu reached. AIRPLANE logcat: SAME "Could not contact DNS server" shape
  as our harness, surfacing as error code 6, menu reached. PROVES a graceful
  catch exists in the binary: identical failure throws here, degrades there.
  Consequence: host unwinder (TTUNW) is THE menu path, not TLS success.
- HEALTHY STATE: phone fusion.registry (320B vs our 144B) READ+rewritten by
  the game (not rejected -> registry crypto not device-bound). Seeded
  work/files5 with phone files. Still throws in NONET (state alone no unlock).
- NONET FIDELITY (fixed, was unfaithful): socket() already succeeded; now UDP
  connect() succeeds (Linux sends no packets; getsockopt SO_TYPE gate), while
  send/sendto/write-to-socket fail ENETUNREACH (host uplink no longer leaks
  under NONET). Resolver now goes connect-OK -> send-fail like on device.
  Result: still "Could not contact DNS server" + throw (shape now faithful).
- LOGO FORENSICS (filmstrip HARNESS_SHOT_EVERY=N): 100 input-less updates =
  Rovio logo, zero change (proven stuck, not slow). Renders don't advance
  (update+render film still Rovio). nativeResume (0xC23DC, dynsym) changes
  nothing. Inputs (tap-to-skip) are the only in-harness advance. Phone: 3s
  logo timeout (user-observed) = plain timer starved of game-time here (idle
  updates take ms; TLOG: early updates dt 100-250ms). No missing engine:
  frames render every update; pacing ~5-10fps is ours to drive. SHOW window
  is black (offscreen render; presentation unwired — screenshots are truth).
- JNI GAP closed: nativeResume needs CallBooleanMethodV (fn#38) =
  AudioOutput.startOutput()Z -> implemented (1=true, default 0).
- TTUNW2 (per-frame unwinder emulation): exidx binary search in C++ (prel31),
  live frames w/ slots, static map ttslots3 (17.5k: 3858 typed + 13729
  catchall/cleanup, incl filter 0). ttunw1: 4348 fns, 20 frames, catchalls in
  0x18953C found BUT pads were fn-RELATIVE (resumed 0x208 garbage — fixed to
  absolute). Pass order: typed matches inner->outer, then catchalls;
  tried-set (24) iterates rethrows; r0 kept, r1=filter (0 catchall), sp kept.
- TLS (closed as menu path, kept as diagnostics): cipher sweep 0x0A/0x2F/
  0x35 all NULL-lookup (reason 247); ECDHE loopback-green but game RSTs
  (no parse crash markers; worker dies by unwind as usual). Real cloud alive
  (TLS1.3, valid cert) but API 403 even with byte-exact real request bodies
  (replayed via python) — anonymous session endpoints dead in 2026.

## 23. Session 2026-10-11 (part 14): TTUNW live, translate mapped, sp hunt
- TTUNW2 live results: 4348 fns, 20-29 frames/throw (FP-walk + BL-scan union;
  exidx search in C++). Innermost typed matches in job runner 0x19FE80
  (filter 2 = Http ti, ~43 pads). Outer frames: typed f1->3 in 0x18766C,
  catchalls in pool/dispatch frames (0x38A0E8, 0x2ABF40, 0x3889EC, ...).
- SUBAGENT on 0x19FE80 (PROVEN): NO graceful pad — all 43 Http pads share one
  handler (translate Http -> rcs::Exception ti 0x65FDB4 + rethrow); zero
  job-state stores, zero notifies. Killed the 43-pad grind.
- Two-level choreography: Http -> hub 0x1A0A3C (path A) -> rcs::Exception
  -> outer catch. BUT path A derails: BAD memchr(0x167d,'}',...) from string
  fn 0xEE4B0, sp-INDEPENDENT (6 offsets fp-28..fp-828 identical) -> not sp.
- EXDUMP (object layout, live): Http obj = [+0]=0x66A5C0(vt?) [+8]=0x31
  [+12]=0x28 [+16]=heapptr [+20]=2 [+24]=double 6.0 (error code!)
  [+44]=1 [+56]=0x51 [+60]=0x42. No 0x167d inside -> handler misreads offset.
- 0x38A2B0 catchall TOXIC (BAD @0x459EF0, both sps). 0x187830 handler throws
  secondary EE4swap (type TBD) at 0x18788C regardless of sp.
- TTPAD batch (6 parallel): outer catchalls (2abfd4/388b54/19fa84) + typed
  (1a0e88) all convert throw -> thread return (-1/0x167d), main still idles.
  Workers one-shot (0x38C048 wrapper, no loop): pad continuation skips the
  job's completion-notify -> main starves. Thread survival != job completion.
- SHOW window black (offscreen only); filmstrip is truth. TLOG dt 100-250ms.
  Logos: 100 input-less updates = stuck Rovio (proven); renders/Resume don't
  advance; taps (tap-to-skip) only in-harness path; phone 3s timer.
  CallBooleanMethodV (fn#38) implemented (AudioOutput.startOutput->1).

## 24. Session 2026-10-11 (part 15): ranges vindicated, msg hunt
- CALL-SITE RANGES (breakthrough): pads carry [st,ln) coverage; our site
  (0x53C in 0x19FE80) is covered by NOTHING previously tried. Full 29-frame
  chain has exactly ONE covering pad: 0xD5EF0 (f1) at frame 0xD5DE4 (blx
  call site verified). All 0x167d pads were out-of-range (wrong locals).
  Range check now enforced in UnwNext (lenient ret/ret-4); ttslots4.txt.
- 0xD5EF0 runs CLEAN (no crash, any sp) but swallows (thread exits with obj,
  no rethrow, no completion). Its ttype slot resolves to PLT garbage
  statically (encoding TBD) — handler runs anyway under emulation.
- 0x1A0ACC (hub off 0xC4C) DOES cover our site ([0x514,0x53C) via call 0x538)
  — range-legit, yet path A derails identically (0x167d) across 8 sp values
  + zero page + faithful regs. Excludes pad-selection AND temporaries.
- EXDUMP (Http obj): [+0]=vt [+8]=0x31 [+12]=0x28 [+16]=heap [+20]=2
  [+24]=double 6.0 (error code!) [+44]=1 [+56]=0x51 [+60]=0x42. No 0x167d.
- Halt-sync clobber PROVEN (pre-run diag: park-time r4/r6 rewritten by halt
  sync) -> ParkUnwind applies state at resume (all paths). FP chain
  validated arithmetically (868+28 exact, saved lr exact).
- SUBAGENT #2 (0x38E9D8 loop, PROVEN): r5 latched once from entry r1; only 2
  callers of 0x38E228; single r8 latch; bound=[sp,#12] maintained by reserve;
  cap-doubling; template embeds ELF bytes; +0x210 = allocator stride (not a
  stepper). Conclusion: 0x167d is a data-dependent int-for-string confusion
  AT ENTRY (msg already poisoned) — not created in the loop.
- NEXT: full msg dump (BIGDUMP=1, 256B) at 0x38E9D8 entry to see the poison.

## 25. Session 2026-10-11 (part 16): RETSUCC success path live
- DEVICE clean-boot-offline PASSES (user-tested) -> our seeds suspected.
  Provenance hunt: empty dir dies on highscores.lua IOException; +lua files
  dies on registry IOException. Missing file = throw (needs catch); any
  registry (0B touch OR phone's) -> cloud attempt. First-launch-offline
  theory DEAD: cloud is unavoidable, graceful-catch is the only offline door
  (or success fabrication).
- RETSUCC (raiser-entry success synthesis, PROBE3): fills response struct
  (status 200 + heap JSON string, E1 tag copied from live strings) and
  resumes caller. Time (epoch+test-sig) + sessions + apdrive (real 1904B
  variantconfig.bin). URL-suffix gated.
- SUBAGENT #3 (sessions): status 200 + keys profile+config (lookups PROVEN,
  handler 0xD6C78, same JSON parser). Inner schema unknown.
- BREAKTHROUGH: fabricated sessions JSON ACCEPTED by real parser ->
  KeyNotFound names the next missing key ("segments" first, via EXDUMP of
  the exception object). Schema iteration: each run names one key.
- NEXT: sessions JSON with segments -> next KeyNotFound (or completion).

## 26. Session 2026-10-11 (part 17): sessions schema solved, clean completion
- RETSUCC (success synthesis at raiser entry): status 200 + heap JSON body
  (E1 tag pattern copied from live strings) + resume caller. URL-gated:
  /time (epoch+test-sig), /sessions, apdrive (real 1904B variantconfig.bin).
- SUBAGENT (sessions): status 200 + profile/config lookups (0x2AC65C).
  SUBAGENT (segments consumer 0x19D034): TOP-LEVEL segments Array<Number>,
  userAuth{accessToken:String,refreshToken:String,expiresIn:Number};
  type ids Null0 Bool1 Number2 String3 Array4 Object5; per-element Number
  assert; empty skips; joined into output+24. Correct JSON:
  {"segments":[0],"userAuth":{...},"profile":{},"config":{}}.
- ITERATION LOOP (works!): fabricated JSON -> real parser consumes ->
  KeyNotFound/BadType names the next gap (segments -> userAuth ->
  segments-entries-must-be-Numbers). EXDUMP reads the key from the exception.
- BREAKTHROUGH: sessions now COMPLETES CLEANLY (workers return 1/2, zero
  throws, registry grows 320->464B with saved tokens!). Main still idles:
  time/apdrive never attempted; next: who queues them (main pickup of
  session state? session-global hunt via 0x19D034 entry regs).
- PROBE5 added (extra dump slot; PROBE3 shared by RDUMP/RETSUCC/TRCALL).
- CORRECTION (subagent): r2-tag (0..4) = HTTP method tag, not success flag;
  success = normal return + response struct [r0+0..48] (status+body+...).
  RETFAIL's "flag=0" actually zeroed an SSO string (empty-response path).

## 27. Session 2026-10-11 (part 18): menu gate found, poke white
- SUBAGENT (gate): poll = [loading-manager+164] at 0x283C34 fork (0->state
  1/menu, else 3/loading); session writers (+120=1/+32/+36) have NO static
  edge to +164 (structural stall); POKE180 torn (confirmed live: 6 fires).
- POKE164 ([r10+164]=0 at 0x283B5C entry): fires, leaves LOADING -> WHITE
  screen with border artifacts (uninit buffers), stable all run. Too-early
  flip whites (content never builds); gated on RETSUCC success now.
- Sessions completes cleanly (workers retval 1/2, registry 320->464B with
  tokens) but time/apdrive NEVER attempted (no new threads; 150 updates no
  change): not timer/count. +16..48 zero-fill tried (retval shifts, no chain).
  expiresIn 9999999999 wraps 32-bit (2014!); 2000000000 used.
- NEXT: gated-poke film (loading built -> flip -> menu?); else find apdrive
  queue trigger / main's session poll target.

## 28. Session 2026-10-11 (part 19): update gate opened (r0=0!)
- TLOG r0: nativeUpdate returned 1 (idle-bail) EVERY update. Gates (subagent):
  #1 *0x672E98 (manager, non-null live PROVEN), #2 vcall+148, #3
  [manager+1187]==1. Gate#3 byte reads 0 live (PROVEN) -> all updates bail.
- Writer 0xBE704 (single in binary: mov r2,#1 + strb +1187 + str +1188, no
  callers/refs -> virtual/computed dispatch). NOT Resume (tested, still 0).
- POKE1187 (+1187=1) + POKE1188 (+1188=1) at 0xC2144: updates now return
  r0=0 (FULL body runs!) but screenshot still LOADING. Body runs yet keeps
  +164=2 (loading): its session checks fail silently.
- NEXT: trace the body's session checks (0xC21EC strings + 0xC5104 vcall
  result) with the gate open.

## 31. Session 2026-10-11 (part 22): registry crypto broken, one-shot dynamics
- DEVICE FORENSICS (rooted emulator over SSH, read-only + mem):
  load bias 0x12EB6000 (deterministic twice); manager *0x672E98 live;
  gate#1-manager+164=0 (matches ours); live POST /sessions (518B, real
  clientSalt/Signature); response-ish {200,200,...} struct; session-state
  struct (session/time keys); healthy 320B registry pulled (work/dreg_device320.bin);
  thread cast (Configuration/NetworkTime/AsyncHttpQueue/TaskDispatcher work/
  ServerLogger/Analytics/Ads/Thread-N); futex topology (waits never signaled,
  broadcasts to empty, waiter 0x9B203224 across boots). NO strace marathons
  (killed game twice); passive reads only. GDB: no arm gdbserver in NDK.
- REGISTRY CRYPTO (subagent, VERIFIED by decrypting both myself): AES-256-CBC,
  zero IV, app-fixed key (NOT device-bound). KEY hex 32B:
  3a7d2e0379e64985a01fa80104d5d77da1bc7ae70363248e7ac9c0ad5f4660ea
  No 32B header (first-32B-identical = CBC determinism on same prefix). Format = JSON + Crypto++-style random pad.
  DEV: real UUIDs, Apprater block, NO cloud section. OURS: cloud{...fake tokens},
  installationID "0", zero pad (our lrand48 flaw, cosmetic).
  SMOKING GUN: decomp FUN_000eabd4 throws underflow_error("Missing or invalid
  refresh token, automatic login not available") — token PRESENCE enables
  auto-login (network); ABSENCE → offline path. Our fake token forces login path.
- ONE-SHOT DYNAMICS (PROVEN): r5 (0xD6F90) = STORED refresh token length.
  EMPTY registry -> r5==0 -> success path (flag 1 via mov). Registry WITH
  tokens -> refresh-fetch -> offline fail -> flag 0 (jump-in with r0=0).
  Every run's save poisons the next (496B vs poison-416B which kills update#1;
  files12_snap.tar + work/clean_registry.bin forged via key, DEV shape, fresh
  UUIDs, 224B; reset before EVERY run!). Device clean-boot-offline == fresh path.
- COMBO2 (fresh+flag1+latch+condsatisfy+spurwake+taps+film): NO abort, worker
  survives looping (spurwake x3), exit flag=1 HELD, film LOADING. +164=2.
  (CORRECTION 2026-10-11 audit: failpath2 (no RETSUCC) = writes 1@D70FC,
  flag=1 — free fresh path, RETSUCC not needed for the flag. The 0@D7100
  writes belong to airplane1.log (token registry), not failpath2.)
- PACING (PROVEN): main sprinting (3s) murders workers via _exit before they
  reach raiser. HARNESS_PACEMS=N sleeps after each JNI (deterministic).
- ROLE MAP (setname, live): tid2 Configuration, tid3/6 NetworkTime, tid5
  Analytics, tid7 Ads, tid9/11 TaskDispatcher work (sessions), tid10 Assets
  (apdrive), tid12 ServerLogger. AsyncHttpQueue (device-only) never spawns
  (ads backend? deprioritized).
- DNS TIMELINES (tid3=time, tid10=apdrive): sscanf URL -> socket(DGRAM) ->
  connect(8.8.8.8:53) -> send(33B) -> (DNSALLOW: real 140B reply recvd!) ->
  TCP sockets -> connect fails 101 -> strerror error-handling -> poll-park.
  EXACTLY ONE DNS cycle each (no retries observed) -> single-shot waits.
- SIGNAL PROTOCOL (decomp FUN_0039f2a4/0x39F170/0x39F21C): predicate =
  mutex->__size[8]&1; set writes 1+broadcast; reset writes 0; wait aborts on
  nonzero (Signal.cpp). CONDSATISFY corrected to mutex+8 (was Link+8, wrong).
  (CORRECTION 2026-10-11 audit: signal-set fn is file 0x38F170, NOT 0x39F170
  (that was a Ghidra-VA/file-VA mixup); predicate itself CONFIRMED via
  FUN_0039f2a4 — premise holds.)
- MCSM COMPARE (subagent, verified): same pattern (guest assets + arm64 host +
  JNI_OnLoad + thunks), cannot explain stall (offline SDL vs rcs handshake).
  REAL gaps adopted: pthread_rwlock_* forwarding (implemented, mirror mutex),
  packaging notes (assets/guestlib + max-page-size later). Guest exports own
  __cxa_* (like MCSM stub).
- 0x74168 CHAIN CLOSED: zero callers of 0x74168/70/74 (all kinds) -> STACKBT
  frame was stale-lr artifact. Gate setter dead in practice.
- OPEN (narrow): post-TCP-fail park (single-shot waits, no backend posts);
  main never observes sessions (no flag/pipe reads); menu scene never
  activates (r1=1 sites never translate). NEXT: main-observation channel.

## 32. Session 2026-10-11 (part 23): update-body checks sterile, full reread
- FULL REREAD (all 1297 lines, by ranges): section numbering is broken
  (28->31->30, no §29 heading) — navigate by CONTENT, not numbers. Re-bitten
  by zsh (`echo ===` breaks it); shell analysis goes in script files.
- PROBE_UPDATE1 (clean registry + paced + PROBE4=c21f8/PROBE5=c2240/PROBE_N=200,
  work/probe_update1.log): flag=1 held, +164=2, film = real RIO LOADING art.
- RESULTS (PROVEN, ~25 updates, pairwise chained: odd-hit r0 == next even-hit
  r4, e.g. hit2 r0=f100322c -> hit3 r4=f100322c): c21f8: r0=fresh f10032xx
  (+4/update = bump-alloc no-reuse, normal), r1=0, r2=manager 0x140E2205
  STABLE, r6=BSS 0x672E6C; c2240: r0=0 (quit-poller NULL every time),
  r1=r2=0x140E2205. ZERO variance across updates.
- VERDICT: update body does NOT observe sessions — sterile normal path
  (stable manager lookup + NULL quit + r0=0, same shape as device must take).
  +164 stall is NOT here. NEXT: apdrive completion delivery (0x28E328 queue
  format / main pickup) or menu-scene activation trigger.

## 33. Session 2026-10-11 (part 24): airplane-with-cache fails locally, no throw
- GHIDRA VERDICT (user: analysis ran with ALL options incl. red-warned ones):
  NO redo needed. Decomp is hypothesis-only; every load-bearing claim is
  live-verified (addr math in python, behavior via probes). Where checked
  (sessions flow, Signal predicate mutex+8) it is exact. Redo only if a
  decomp-vs-live contradiction blocks us.
  REFINED (user clarified: ALL boxes ON, screenshot showed defaults for
  orientation): xref coverage is MAXIMAL (Data Reference + ELF Scalar ON) ->
  static "zero callers/refs" claims are STRONGER than default-Ghidra work.
  Remaining risks are only the red Prototypes: phantom functions from data
  bytes (Aggressive Instruction Finder — mitigate by distv-cross-checking any
  "orphan" before calling it dead) + cosmetic filler condensing + variadic
  signature display (vararg calls verified live anyway). If a redo ever
  happens: prototypes OFF, everything else as-is.
- AIRPLANE1 (files17 = seeds + files12 496B token-registry, pure NONET, no
  DNSALLOW, combo otherwise; work/airplane1.log): job#1 (tid9) reads 496B
  registry (tokens present -> r5!=0 -> refresh path), gathers device info
  (cpuinfo, Build.MODEL/RELEASE, UUID), rewrites 496B .tmp, then writes flag
  0 at 0xD7100 — NO network attempt, NO throw (zero `throw type` in run).
  Job#2 later reaches raiser -> RETSUCC 200 -> parsed, flag stays 0.
- MECHANISM (disasm 0xD6F80..0xD7088): refresh block ends in fall-through
  `mov r0,#0` (0xD7084) -> fail-cleanup 0xD7100; success jump from beq 0xD7078
  -> 0xD708C. Gates: ldrb [sp+324]/[sp+336] + tst #1 (SSO/presence shape TBD).
- IMPLICATION: token-registry + offline != device menu path. Device's
  airplane catch must key on something else (DNS-throw path? error-code-6
  surfacing?). NEXT: live branch probe (airplane2: PROBE4=d7084/PROBE5=d708c)
  + static refresh-block decode (background subagent ses_ed405048effeJSVcwBodXmm8KM).
- AIRPLANE2 (work/airplane2.log): d7084 translated (fail fall-through RUNS),
  d708c never translated (success block dead); flag 0; registry byte-identical
  after run (496B, fake tokens persist — one-shot poison reconfirmed).
- ROLES NOT PINNED: apdrive sscanf fired on tid11 here (was tid10 before);
  pool threads run jobs SEQUENTIALLY (tid11 = sessions-succ job, then apdrive
  job). setname labels are per-run, not architecture.
- DNS-FAIL SHAPE (pure NONET, tid11/tid3): socket->UDP-connect-OK->send 33B->
  silence->close,close->other work (time/sigaction/ioctl)->poll-park. NO throw
  anywhere (zero `throw type`), NO 'Got exception' in ANY log (airplane2,
  combo3, paced1 all 0). Our error path ends jobs WITHOUT completion-notify;
  device must throw+catch here (error 6) — divergence point unproven.
- ASKED USER: exact offline methodology on emulator + offline-boot logcat
  markers (DNS/error-6/sessions/apdrive lines) + cache state of that boot.
- ANSWERED (user): airplane on host+guest + wiped data; works with/without
  cache, with/without network. Device clean-boot-offline == our combo2 shape
  (fresh registry + offline) -> divergence is purely time/apdrive fate.
- LOGCAT GOLD (rio_boot2.log, user's file, READ-ONLY): offline boot logs
  3x `E RovioAccount: Login failed: -1 => HTTP Exception: HTTP request failed
  with code 6: Could not resolve: cloud.rovio.com (Could not contact DNS
  servers)` — then continues to menu. PROVEN device DNS shape: queries
  UNANSWERED (not send-error) -> timeout -> HttpRequestException(6) -> caught
  (Java logs Login failed) -> fallback -> menu.
- DIVERGENCE (INFERRED, sharp): our NONET fails DNS sends IMMEDIATELY
  (ENETUNREACH) -> silent-park branch; device sends (or attempts) and times
  out -> throw branch. FIX: HARNESS_DNSHUSH=1 (swallow DNS sends as fake-full,
  no reply -> resolver timeout; implemented in send path, rebuilding).
  NEXT: dnshush run on clean registry (exact device config) — watch for throw
  (type? caught? thread death?) + flag + film.
- DNSHUSH1 RESULT (work/dnshush1.log): 13 swallowed sends (t3 x7, t11 x6 —
  resolver RETRIES on timeout, unlike immediate-fail single-shot) then park;
  NO throw, sessions flag=1 (RETSUCC, transport-independent), +164=2.
  Unanswered-DNS alone does not reproduce the device throw.
- CORRECTION (PROVEN by own check): Ghidra VA = file VA + 0x10000 (NOT equal).
  Verified: FUN_0006eba4/0006fe70 -> file 0x5EBA4/0x5FE70 land EXACTLY on
  stmdb prologues (distv). §30's "FUN_ == file VA (verified)" was a bad
  verification — lesson: re-verify VA claims by prologue/bytes. Rule: subtract
  0x10000 from any Ghidra code VA to get file VA.
- LOGCAT-ANCHORED CHAIN (bytes): "Login failed: %d => %s" fmt at file 0x5C0C0F
  == logcat text exactly. Logger = loginFinished callback (file 0x5D2F0):
  param_2==1 success (promptLogin...), else "Login failed" log + continue.
  Callers: success functor (file 0x5EBA4) -> loginFinished(ctx,1); failure
  functor (file 0x5FE70) -> loginFinished(ctx,0). OUR RUNS: neither fires ->
  jobs die/park pre-completion. NEXT: probe both functors live
  (PROBE4=5eba4/PROBE5=5fe70) in dnshush run.
- FUNCTOR1 (work/functor1.log, dnshush, PROBE4=5eba4/PROBE5=5fe70): NEITHER
  functor translates (certainly never runs). BONUS hit #5: live std::function
  object of class `RovioAccount::loginRovioId...$_0` passed at a probed site
  (lr=0x5E270, r1=functor addr) — completion machinery is SET UP, never fired.
- PIPE TRUTH (live, 4 events/run): main(t1)->t11 and t4->t3 wakes (job ASSIGN);
  ZERO worker->main wakes (no completion reports). Main assigns, workers run
  (sessions parses, flag=1) but never post results; main never reads flag
  (sesswatch zero reads) -> circular stall; loading scene re-asserts +164=2
  every render.
- FUNCTOR1 (work/functor1.log, dnshush, PROBE4=5eba4/PROBE5=5fe70): NEITHER
  functor translates (certainly never runs). BONUS hit #5: live std::function
  object of class `RovioAccount::loginRovioId...$_0` passed at a probed site
  (lr=0x5E270, r1=functor addr) — completion machinery is SET UP, never fired.
- PIPE TRUTH (live, 4 events/run): main(t1)->t11 and t4->t3 wakes (job ASSIGN);
  ZERO worker->main wakes (no completion reports). Main assigns, workers run
  (sessions parses, flag=1) but never post results; main never reads flag
  (sesswatch zero reads) -> circular stall; loading scene re-asserts +164=2
  every render.
- NOTIFY VERIFIED BY BYTES: file 0x28ED64 = notify (r4=r0 queue, [r4+40]=wfd,
  strb 0 + write); file 0x28DD04 = enqueue entry. NEXT: notifymap1 run
  (PROBE4=28ed64/PROBE5=28dd04) — who-notifies-whom + queue addrs live.
- FAILPATH TURN (no RETSUCC, nodns shape): sessions POST fails -> THROW ->
  unwind (tid9 done=1, last=__gnu_Unwind_Find_exidx, retval=0x45A778) -> t9
  POSTS pipe wake (t9-write gfd157 -> t10-read gfd156) -> thread dies. Device
  path unfolding! BUT no catch lands (5fe70 never translates, no Login-failed)
  -> t10 swallows wake, parks. HYPOTHESIS: wake posted WITHOUT queue entry
  (queue-checked reader swallows) — verify via enqueue probe.
- EXIDX RECIPE from old notes did NOT reproduce (garbage fn addrs) — dropped,
  marked untrusted. Ghidra-0x10000 delta stands (3 independent prologue hits).
- NEXT: failpath3 (enqueue 28dd04 + fail-functor 5fe70 probes, no RETSUCC) —
  does the exception path enqueue? does t10 dequeue? does functor fire?
## 30. Session 2026-10-11 (part 21): Ghidra decomp breakthrough
- User ran aggressive Ghidra analysis -> Ghidra_AngryBirdsRio/out/libAngryBirdsRio.so.c
  (23MB, 75511 FUNs). Ghidra VA = file VA + 0x10000 (CORRECTED 2026-10-11:
  old "FUN_ == file VA" claim was wrong; FUN_0006eba4/0006fe70 -> 0x5EBA4/
  0x5FE70 prologues prove the delta). .rodata DAT_ mapping: use file VAs, Ghidra splits fns at
  tail calls (e.g. 0xD6C78 inside FUN_000d6bfc) — request decomp by inner addr.
- DECOMP WINS (sessions fn ~line 92900): uVar2/+0x1E = fresh(1)/refresh(0)
  ROUTING (not success!). +0x1E=1 -> THROW "Login failed: "+underflow_error
  (LAB_000e7aa0) = our unwind deaths (0x45A778). +0x1E=0 -> bVar1=true, skips
  logger gate, returns normally. Logger gate (len 4 + memcmp "true") CONFIRMS
  our sdkDebugLoggingEnabled shape. r8+28/+40 cleanups = 0x72530 (our crash!).
- "access"(6)/"token"(5)/"refresh"(7) lookups read STORED creds (registry),
  NOT the response -> adding keys to sjson was useless by construction
  (access1/2 runs confirmed no change). Throwing getters: segments,
  accessToken, refreshToken, expiresIn, message. Tolerant: profile/config.
- r5-empty success path (beq 0xD6F94): r5 = STORED refresh token length.
  Registry WITH tokens -> r5!=0 -> refresh-fetch -> offline fail -> flag 0.
  Registry EMPTY (fresh boot) -> r5==0 -> flag 1. REGISTRY IS ONE-SHOT:
  every run's save poisons the next (496B vs poison-416B that kills update#1;
  snap rollback works). Device clean-boot-offline == fresh-registry path.
- Subagents (3 parallel, all completed): menu-trigger (writer/callers/scene
  table 0x400B0 + viewport guard mapped), reader protocol (VALID = queue
  entry + wake; 0x18D734 NOT enqueue, real one 0x28DD04), sessions-fail
  (above). Rate-limit hiccup resumed via sessionID (no recreation needed).
- LOADING animation hypothesis (user): REJECTED — frames byte-identical
  across dozens of updates (md5), clean returns, no crash markers. Static
  hold of state machine (+164=2), not crash.
- NEXT: satisfy tid9's Signal cond (predicate [Link+8], Link in r4) via
  HARNESS_CONDSATISFY (set flag + return 0) + latch + spurwake; watch for
  post-cond notify (+164? new threads?).
- COMBO2 BREAKTHROUGH (live, PROVEN): fresh registry -> tid9 writes 1 (SUCCESS
  PATH via mov, not jump!) -> cond SATISFIED at correct mutex+8 (decomp
  FUN_0039f2a4 proved predicate = mutex->__size[8], NOT Link+8; old satisfy
  wrote wrong place) -> NO abort. tid9 survives, loops (spurwake x3, awaiting
  jobs). exit flag=1 HELD. +164 still 2, film LOADING.
- r5 = STORED refresh token: registry WITH tokens -> refresh-fetch -> offline
  fail -> flag 0; EMPTY registry -> r5==0 -> flag 1. Registry is one-shot
  (each run's save poisons the next; 416B poison kills update#1, snap
  rollback works). Subagent: registry crypto field diff (device-320B vs
  ours-320B) to find what main waits for.
- 0a SLOT FIX (real typo): pre-call poke used 0x672E9C (INVALID obj every
  call); now 0x672E98, obj=10003250 matches mid-update hook (PROVEN live).
- 0b RETSUCC BUDGET: `++n>4` counted unknown endpoints (burned by retries);
  now `calls` (log) vs `n` (injected only, cap 64).
- 0c POKE1187-ALONE holds r0=0 on ALL updates (no 1188/164 needed) —
  +1188=1 was unnecessary (P3 supported). Sessions 2x clean, zero throws.
- QUIT-BRANCH EXCLUDED: body reaches 0xC223C bl 0xC524C (quitRequested
  poller); +912 vcall returns NULL every time -> clean early return, no
  throw. quitRequested chain completes silently BY DESIGN (nothing requests
  quit). Session/time/apdrive logic is NOT in this branch.
- Body start: 0xC2160 bl 0xC1254 with BSS-resident key ptr (add r1,pc ->
  0x672DF0, file-empty). Phase-1 probe: WPA=672df0 + TRCALL=c2160,a0.
- PHASE-1 RESULTS (live, PROVEN): 0x672DF0 written once at init (0 ->
  f1003008 by 0x2F9690); read every update from 0x2F971C (main DOES poll
  the slot). f1003228 = GetObjectClass(f1003008): C5104's first arg is a
  jclass (Java method lookup quitRequested ()V). C5160-block r0 =
  f100322c..325c non-null x16 (vcall+132 SUCCEEDS, no MemberNotFound).
- 160x `throw type: EE4swapERS3_` (20/update, caught internally, r0=0) —
  raiser hunt via lr print added to throw line (throwlr.log).
- SUBAGENT-ERROR CAUGHT (user asked to verify): surf vptr 0x669404 =
  gr::gles2::GL_Context (vtable-4 typeinfo PROVEN), NOT ShadowingSurfaceData.
  +180 is NOT a state field (garbage). st==3 flip-gate REMOVED (it could never
  open); POKE164 runs never actually flipped anything. Rollback: b6f185a^.
- RENDER HYPOTHESIS DEAD: 16 nativeRender calls execute clean, sessions 2x
  clean, but state stays +164=2, surf unchanged. Screenshot = Rovio LOGO
  (not loading!): short runs never leave the logo phase.
- TAP INSIGHT (§12 reread): 20 updates no-tap = Rovio splash, 20 with tap =
  Fox splash — taps ADVANCE boot. Current run: taps (JOVR down/up center)
  + renders + RETSUCC + POKE1187 + DIAG164, no POKE164 (watch natural +164).
- TAP BREAKTHROUGH (PROVEN filmstrip): taprun1 shot008=Rovio logo,
  shot032=FOX logo, final=FOX. Input (JOVR down/up center 400,240) advances
  boot with real frames. +164 still 2 (loading not yet built).
- HARNESS_POKE164_DELAY=N added (flip on/after Nth firing; early flips
  white). Long run: 27 updates + 27 renders + 3 tap pairs, DELAY=10.
- FLIP KILLS BOOT (PROVEN film): longrun1 shot008=Rovio, shot024+=WHITE.
  Forcing +164=0 with unbuilt content whites everything (taps never even
  reach Fox). POKE164 shelved; natural path only.
- NATURAL BREAKTHROUGH (natrun1, no flip): Rovio -> Fox -> Blue Sky ->
  RIO LOADING with real art (Blu + landscape + LOADING text)! Taps advance
  splashes; sessions 2x clean; /time + apdrive sscanf fire first time.
  +164 stays 2. Remaining: LOADING -> menu (time/apdrive complete?).
- STUCK-WORKER HUNT: 10 created (tids 2-11, all start=0x38C048), only 4 done
  (4,8,9,11). 6 hang: time/apdrive sscanf but never reach raiser (no succ,
  no throw). HARNESS_WBEAT=1 added (per-worker PC sampler, wbeat1.log).
- WBEAT+HISTories: all 6 park in poll() on PIPES (h12/14/16/18/20/23, POLLIN
  1s loop), never sockets. Main created all pipes, holds write ends.
  tids 2,5,6,7,12 = spares (bootstrap-only SVCs, never assigned).
  tid3=/time + tid10=apdrive: assigned (1B wakes), did DNS sends, park.
- PIPE PROTOCOL (PROVEN): 1B 0x00 wakes only, no data messages. Queue validated
  ([r4+44] count). NVAL/ERR lies swallowed (queue-checked reader 0x28E328).
  Pipewakes consumed without progress. Backend poster doesn't exist in runs.
- +164 WRITERS: callers pass r1=1 (0x2506B4/0x250DB0/0x2511D0, menu fns, ZERO
  BL callers = virtual) vs r1=2 (0x260B88/0x261378, loading, runs every
  render). r1=1 site 0x250680+ never translates (PCTRACE-proven). Menu scene
  never activates. Flip futile (overwritten next render, PROVEN longrun1).
- DNSALLOW BREAKTHROUGH (HARNESS_DNSALLOW=1 punches UDP/53 past NONET):
  real 8.8.8.8 replies (140B) recvd by workers (tid3/time first); TCP legs
  fail fast (101); time/apdrive enter ERROR HANDLING (strerror+errno hists)
  instead of poll-park. Needs time to complete -> 5-min run going.
- TLS (all proven live): DNS 140B replies; TCP:443 connects; entropy
  (urandom), cacert parse (77B memsets); ClientHello 198B (record 0301 +
  body 0303); ServerHello 5B+110B read; client sends fatal alert 47
  (illegal_parameter) -> reason(247) HttpRequestException. Root: 2014
  stack capped at TLS 1.0 (any 0303 ServerHello record aborts before body
  read). Downgrade offer 0301/0301 -> server silent (CF min-version 1.2+).
  Hand-rolled ClientHello parse: modern ciphers offered (CHACHA/GCM) but
  ServerHello still rejected (extensions or curve — subagent on alert site).
  Cipher-alert proof vs CF (alert 40 no-overlap). Fake origin works
  (hosts-hack honored! portmap 443->8443) but same alert-47. ONLINE PATH
  DEAD both directions (crypto impossible + API dead 401/403).
- BLOCKING SOCKETS (real bug, fixed): O_NONBLOCK caused short reads (5B of
  115B ServerHello) -> alert. Now blocking + SO_RCVTIMEO/SNDTIMEO 10s;
  poll-caps removed for socks (FIFO cap kept). getpeername/getsockname
  real (sa_addr ntop chain). fcntl real (O_NONBLOCK honored).
- REDIRECT (throw->pad emulation, works!): ProbeRedir at throw entry +
  per-Jit resume (USER+ARM, r0 kept) + wrapper loops (cap) + REDIR=pad
  chain + REDIRN budget. Proven: Http->0x28E7C4->length_error (begin_catch
  accepted r0=obj!). BUT all dispatcher pads rethrow (translate pattern:
  Http->length; length->length/swap). 117 begin_catch pads mapped; every
  pad-function also throws (no pure handlers statically). Chains tried:
  [28e7c4,189ef0], [28e7c4,2848c4] — both rethrow. r0/r1/sp questions
  remain (r1=switch? sp=scratch?). NEXT: app-level catch (main-side?) or
  real unwinder (phase1 driver + opcode interp + personality hijack).

## 34. Session 2026-10-11 (part 25): subagent waves + nopoke kills poke
- POKE THEORY DEAD (work/nopoke1.log, full combo WITHOUT POKE1187): identical
  stall (flag=1, succ, +164=2, LOADING film, full 63 calls). Natural gate
  changes nothing. POKE1187 kept (harmless) but no longer suspected.
- MARATHON early _exit (work/marathon1.log, 483-call list): guest quits cleanly
  at ~102 calls (census, no BAD). Cause OPEN (tap pattern? update count? NOT
  poke — nopoke ran full length). Waiting-test blocked until understood.
- SUBAGENT WAVE-1 (all read-only, all completed):
  1. Time-forensics: SLOW bg level-preload to 99.7% (new handles + real
     glTexImage2D of INGAME stone/wood/steel); subsystems freeze ~50%.
     Game alive, menu trigger parked.
  2. Assets: loading set COMPLETE (LOADING art/fonts/menu sheets as pairs).
     Early-asset-error EXCLUDED.
  3. Queue: struct mapped (+0x24 read-fd/+0x28 write-fd/+0x2C pending; reader
     +process loop; enqueue sites). CORRECTION: file 0x5E0FC = constructor,
     NOT the loop (real loop file 0x5E164, never runs); calls virtual-only.
  4. Login: reachable ONLY via Lua global native_RovioAccount.loginRovioId
     (Lua 5.1 closures PROVEN in-binary); no native BL callers; no Java.
- INVOKER/LAMBDA/HEAP negatives (live): file 0x5E434 (slot-6 invoker)
  translates in RETSUCC runs; hit lr=0x5E270 carries event-link-shaped object
  (vtable file 0x66B824, self-ref) + loginRovioId lambda as r1 — chain fires
  but functor bodies (0x5EBA4/0x5FE70) never translate. Heap scans clean for:
  fail/success functor code addrs, vtable-A/B users, lambda vtable 0x65E590
  (fail configs), manager vtable 0x65D560 user. Login callback machinery never
  materializes in fail runs. Login poll reader 0x5D838 + all 6 account handlers
  silent. Decrypted Lua in heap references loginRovioId (sendStatsToAccount
  neighborhood) but never invokes.
- RELIABLE DEATH (work/relthrow1.log, servers+sendfail, no RETSUCC): tid9+tid12
  die via unwind 2/2 (Find_exidx, retval 0x45A778). Playground for REDIR.
- WAVE-2 launched (read-only, background): JNI mock audit, time/apdrive
  endings, Lua execution forensics.

## 36. Bets 1-2 (binary outcomes)
- BET 1 (loginRovioId in RETSUCC config): SILENT. Lua path dead in BOTH
  configs. RETRACTED: functor1 hit-#5 "lambda" = class-sniff artifact on event
  memory (handler 0x5D7C8 never translates anywhere, so no lambda object can
  exist via it). Lua hypothesis DEAD by elimination.
- BET 2 (process 0x28DAF4 + loop 0x5E164 in fail config): process RUNS 318x
  round-robin over 7 queues (48/46/45/45/45/45/44) INCLUDING completion queue
  B (46x); loop 0x5E164 SILENT. Swallow is INSIDE process (or downstream).
- Entry shape (failpath3 hits 5,9, both t-posts to B): {objA, objB-0x10,
  MAGIC 0xFB048F99, 0, ptrY, ptrZ, RET 0x28DC18, ptrW} — well-formed job
  records, identical skeleton. [+12]=0 both.
- NEXT (Bet 3): process's per-entry skip-branch — static body read first.

## 37. Bets ledger (binary outcomes, agreed format)
- BET 1 (loginRovioId fires in RETSUCC config): MISS (work/bet1.log — silent).
  Lua hypothesis DEAD in both configs. Retracted functor1 hit-#5 "lambda" as
  class-sniff artifact (handler never translates anywhere).
- BET 2 (process 0x28DAF4 + loop 0x5E164 execute in fail config): SPLIT.
  Process RUNS (work/bet2.log: 318 calls round-robin over 7 queues incl. B
  46x); loop SILENT. Entries well-formed (failpath3: {objA,objB,MAGIC
  0xFB048F99,0,ptrY,ptrZ,RET 0x28DC18,ptrW}), drained with zero effect (B
  oscillates 1-entry/empty live).
- BET 3 (WPA catches B's consumer, work/wpa1.log): MISS (caught only queue
  construction near raiser: begin=0 via str at 0x28A4D4). BONUS: B stable
  3/3 configs (0x12D64DB0); B is per-flow (built under raiser path), shared
  by both sessions posts.
- RULE (safety, agreed): subagents NEVER get dynamic analysis (their runs
  would mutate work/files* registry + heap addrs + shots, breaking my
  determinism). Dynamics only with separate files dir + registry backup +
  my runs paused. Statics get decomp + byte access.
- NEXT: subagent decoding process's callees (0x28E424/0x28E548/0x28F698/
  0x28F76C + far blx) for entry fate: free vs move vs drop + condition
  (ses_ed3888397ffeFbwgN2bjOlT1Pb, background).

## 35. Session 2026-10-11 (part 26): load-bearing audit (8 claims)
- Subagent audit (read-only): 1 Ghidra-delta CONFIRMED (+pairs). 2 flag
  mechanics CONFIRMED with attribution fix (failpath2=flag1-free; 0-writes =
  airplane1). 3 queue struct CONFIRMED. 4 login chain 6/7 CONFIRMED (string
  file-offset unverified, immaterial). 5 RETSUCC linkage: flag-1-free proven
  WITHOUT it (failpath2/3, nopoke1 runs) — RETSUCC adds only token-save
  (poison); succ+flag co-occurrence in one run still open but immaterial.
  6 crypto: RE-VERIFIED BY EXECUTION today (all 3 registries decrypt: valid
  pad 9/13/9 + valid JSON + expected top keys) — weakest link closed.
  7 CONDSATISFY predicate CONFIRMED, file-VA attribution fixed (0x38F170).
  8 worker posts + swallowed reads CONFIRMED literally (t9/t12→t10 pairs).
- Standing: every load-bearing claim now has >=2 independent supports except
  string file-offsets (immaterial). Next intervention unchanged: login
  initiation (Lua) or functor invocation.

## 39. Frida device gold (2026-10-11 night, gadget repack works)
- Gadget route PROVEN: apktool decode + loadLibrary in App.onCreate +
  libfridagadget.so (arm32) + listen :27044, debug-signed; engine md5-identical;
  gadget LISTENS (no ptrace). vphoneos hides engine mappings (no process maps
  the .so); base found by byte-pattern memscan (needle nativeConfig@0xC1550):
  0x147F4000/0x146F4000 per boot (NOT 1MB-aligned — slot probing unreliable,
  full scan required). NOTE: frida sessions lag 30-100s+; read logs AFTER end;
  per-frame hooks destabilize (2 crashes) — sparse/change-only hooks only;
  NEVER hook the throw site (prime crash suspect).
- COLD BOOT CAPTURE (pid 4742, full): flag d70fc writes xN, raiser POSTs xN
  (r2=2), loginFinished(ctx=0x15289710, flag=0) x2 SAME ctx, backtrace:
  loginFinished <- 0x63EE4 <- 0x63B54 <- 0x388904 <- 0xBC100 <- JNI trampoline
  <- libart (JAVA calls native 0xBC100!). [REFUTED 2026-10-12, see §44/A1:
  no such call edges in bytes (0x63B54=tst, 0x388904=add, 0x63EE4 never BLs
  loginFinished) — stale-lr garbage; 0xBC100 = ads addPlacementNative.] 0xBC100 has NO Java_* symbol
  (dynsym-checked) -> dynamic registration (our bridge lacks fn#215 entirely,
  zero fn#215 in all logs). Gate settles to 1. Menu follows.
- IN-MENU (pid 5221/3388): gate164 polls flip 2<->1 EVERY poll (6650 hits;
  object addrs not logged — one vs two managers OPEN); NO loginFinished, NO
  bc100, NO flag writes, NO raiser across minutes of navigation. Login
  subsystem active ONLY at boot. Menu lives without it.
- NEXT: cold boot + bc100-entry regs (signature!) + Java caller + ctx dump.

## 52. Time/apdrive DEAD for boot, 3rd confirmation (2026-10-12, S1 static)- Read-only subagent, byte-verified: time leg = job -> curl-multi -> parser
  0xCBCE0 (200-check, JSON asserts, writes state+0x28/2C/30) -> publish to
  NetworkTime LISTENERS (success functor sets +0x54/delta + cond signal).
  Apdrive leg = local variantconfig parsed at boot (direct ctor, no queue) +
  cloud job parks pre-raiser; listeners + state flip, no queue. NEITHER leg
  has a main-advancing link (no main poller/reader/queue/event in update path;
  sesswatch zero reads; pipes main->worker only). Even completed, main would
  not move. Consistent with 10-subagent synthesis + zero device lines.
- Recipes exist (direct 0xCBCE0 / 0xC8F90+0xC2134 calls) but pointless for menu
  (no observer). Time/apdrive PURSUIT OVER (third independent confirmation).
  Awaiting S2 (gate+148) for the last main-observable candidate.

## QUEUED DEVICE WORK (do not forget; needs live game + single calm session)
- STATE 2026-10-12 (user): original APK installed and ALIVE (gadget repack
  uninstalled; saves reset). User pre-approved (re)installing the patched
  gadget APK when D1/D2 come (repack workspace: work/gadget/, engine md5
  identical). No device action until the no-device queue is exhausted.
- D1. BUILDER-CAPTURE (fast path #1, highest information): one-shot sparseRE (fast path #1, highest information): one-shot sparse
  hook on 0x250AB8 entry at cold boot -> this/vtable/r1/caller-backtrace +
  event-vector state at fire time. Collapses factory+this+key+subscriber hunts
  at once. Preconditions: game alive on emulator, tunnel up, NO flood hooks,
  read logs only AFTER session end. (Queued 2026-10-12; log-tail diff gave
  timing only: login pair +3s, retry +10s, no mechanism.)
- D2. JNI-BOUNDARY DIFF (fast path #3): sparse Call*/Get* names+counts over
  boot->menu -> diff vs our ~10/63 called natives -> call the missing ones.

## CRUTCH LEDGER (harness-only diags, never ship; remove when they stop paying)- EVENTPOST/EVENTFILL (EventEntry/EventDecide/free-marker): synthetic events
  -> walk invokes. Status: PROVEN delivery (INVOKE 27/27). KEEP (infra).
- SCENEAT (7-arg BL 0x283A3C): flip proven live + white. Status: verdict
  recorded (§47). KEEP code, no rerun needed unless builders change the scene.
- BUILDER (+BUILDERFORGE): direct 0x250AB8 with live/forged this. Status:
  FORGE verdict RECORDED (builder2: forged this vtable 0x669428 + zeros runs
  CLEAN hr=UD1 r0=0, film unchanged LOADING, +164=2 — builds into the void,
  no graph attachment). Keep dormant (env-gated); never ship.
- RAWPOST/RAWBUILD/LOGINAT (login calls): all sterile. Status: verdicts
  recorded. KEEP code (cheap), no rerun unless ctx recipe changes.
- POKE164/POKE180/POKE1187: harness-only pokes. POKE1187 kept (harmless),
  POKE164 shelved, POKE180 torn-record only. Never ship.
- RETSUCC (canned HTTP): most successful scaffold. KEEP; ship-candidate ONLY
  as documented offline-session strategy, never as "network works".
- Fill (1a410c,{}): boot-critical scaffold. KEEP until cloud-config has a
  real source. Never ship as-is (needs honest equivalent at packaging).
- Rule: every crutch logs its marker lines; a crutch that fires zero times
  across 3 runs gets removed. SHIP: loader, real thunks, threads, asset
  bridge, build (see §10 PORT LEDGER).

## 53. Gate#2 static-always-1; main observes nothing (2026-10-12, S2 static)- Read-only subagent, byte-verified: gate#2 = [GL_Context *0x672E6C]+148 =
  `mov r0,#1; bx lr` (reads NO memory; vtable fileVA 0x669404 = GL_Context,
  object 0x2C0B heap, created nativeInit, freed only deinit). Live "0->1" was
  null->created, not a state flip. Gate#1 (manager non-null) + gate#3 (quit 0)
  also static. MAIN OBSERVES NOTHING DYNAMIC — trigger is calls, never data.
  Downstream of gate#2 (always taken): time accumulator [+648], manager
  update, EVENT WALK 0xC2140 (empty vector), quit branch. What varies per
  update: [+648] clock, timestamps, manager/event/scene state — never gates.
- Consequence: no completion-file/flag/queue can move main (nothing reads
  them). Remaining menu paths: (a) early login initiation (never fairly
  tested: loginat6 faulted on heap, now fixed), (b) natural event posters +
  subscribers (both absent), (c) scene factory trigger (unknown).
- NEXT (live, no build): LOGINAT=11 + functor probes (5f96c/5fe70) + session
  stash — early-timing initiation retest with 1GB heap.

## 54. Builder series verdict + menu reference (2026-10-12)- builder1: no live this (phases drifted, 0 executions) — loading-this is
  phase-dependent, not reliably present. builder2 (forged this vtable
  0x669428 + zeros): runs CLEAN hr=UD1 r0=0, film LOADING unchanged (builds
  into the void). builder3/4: frustum gate (bl 0x25070C, cmp r0,#1) SKIPS
  (post-gate never translates; predicate args rect=stack-NaN, count=4).
  builder5 (BUILDERGATE r0=1 @0x250C80): body runs, faults writing via
  builder->writer(r1=1) call (lr=250DB4; pc-lag ±1 known) — needs live scene
  graph state. builder6 (+borrowed live render obj): SAME fault (not +0xA8).
  builder7 (tail/store probes): 166 executions all loading traffic (r1=2,
  lr=261378, +164=2 held); ours never reaches tail.
- VERDICT: direct-builder path FULLY MAPPED (entry->virtuals->gate->body->
  writer-call->fault-on-missing-graph-state) and CLOSED as menu path (needs
  live graph; forge cannot attach). Builder code itself proven runnable.
- MENU REFERENCE (user device shots): main menu = Rio landscape bg (SAME art
  as our LOADING) + logo top + ORANGE PLAY DISC center + ToonsTV + gear
  bottom-right + Bird-Coins popup w/ OK (first-run; Apprater block in registry
  is the likely condition) + slight bg animation. Implications: bg already
  loaded (only UI layer missing); film verdict = play disc present (structure,
  not md5 — animation moves); motion in film = menu-alive detector; play disc
  center = post-menu tap target (queued). Popup/Apprater front parked.
- NEXT: factory subagent (scene construction trigger) — only open builder-side
  lead. Login ladder paused (needs session-map state, deeper than trigger).

## 55. Scene vector: 1 loading node, no menu node (2026-10-12)
- Manager state satisfies factory recipe late-run (+164==2, +168==scene,
  +172/+176 nonzero, +216==1, BSS valid; B4 non-empty with loading content;
  scene188 steady 1 entry). DIAG164 extended (fields + vector + node dump).
- Node layout (live, stable 2x): node=heap, [+0]=0x669710, [+4]=child-heap,
  [+8]=[+12]=0, [+16]=1.0f, rest 0 (32B shown; full size TBD by subagent).
- Menu node id 0x101929: 0 heap hits (HEXFINDHEX). Vtable 0x669710 objects:
  EXACTLY 1 (loading node, addr-stable). Menu node does NOT exist (neither
  built-dormant nor linked) — must be constructed (factory) or forged.
- NEXT: node-layout subagent (size? id offset? loop edge? appender? recipe)
  -> forge-or-factory. No live angle until it returns.

## 56. Factory verdict + loopcall + coverage map (2026-10-12)- Factory (read-only subagent): SCENE factory 0x24DB38 (w,h,orient; new 704B;
  vtable 0x669404 = GL_Context!) called ONLY from nativeInit (once). GL_Image
  factories build splash/loading images. NO menu-scene factory exists. Builder
  key 0x101929 = graph node id (dynamic; loop iterates scene+188 calling node
  virtuals). Appender = loop MISS path itself (allocate+construct 0x26305C +
  push/grow). Recipe needs LOGIN-ONLY STATE (mgr fields + loginFinished +0x19
  + sessions/catalog populating scene+188) — the populate step never fires.
- Loopcall1 (direct BL 0x250018(scene, 0x101929)): runs CLEAN hr=UD1, writes
  scene+0x18=7, returns FRESH node* 0x133A2CD0 (miss path built!), next update
  returns node* (update observes loop state!). NO builder translations, film
  unchanged. Verdict: loop acquirable, builders not reached from it (need the
  loop's post-miss invocation path or manager drive, or node activation).
- Cover1 (PCTRACE 0x250000+0x40000, 24k blocks): builders NEVER translate;
  surface loop translates (reached); manager entry/tail/store translate (1x);
  loading twins (0x260xxx) NEVER translate (draw path = 0x263xxx direct!);
  event walk translates. CORRECTS old "twins run every render" (grep artifact;
  all pctrace/emits must be read exact, never pattern-guessed).
- Mgrat series: manager pass needs +172/+176 (pulse timing-jittered; missed
  twice); forged zeros reach an exclusive-write fault @0 (queue-ish object
  needed — loading-side mechanics, deprioritized vs login ladder). Poll-mode
  fixed (fire-once). +216 steady 1 (pass clears, something re-sets or never
  runs naturally).
- Node verdicts: 1 loading node only (vtable 0x669710, layout known); menu
  node absent (id + vtable scans empty). Forge recipe exists but needs graph
  attach (contradicted 0.96 alone). Node subagent pending (size/id/edge).

## 57. Login chain ordered triple + session+72 null (2026-10-12)
- loginchain1 (LOGINAT=25, LOGINAT1=35 r1=1, D5994(r1=2)@40, functor probes):
  initiation clean (+25 0->1 PROVEN via pre25=1); r1=1 jumps wild (pc=0x134,
  sp=unmapped gap — virtual through null, NOT setjmp (no such imports));
  GuestMmap properly maps (gap theory dead); D5994(r1=2) clean, post [+0x18]
  = pre-existing "ault" bytes; functors 0; film LOADING.
- sessdump1 (HEXDUMP session window): session+64..+96 ALL ZEROS => +72 null.
  The r1=1 path's +72 virtual reads null -> module-zero-page deref chain ->
  wild jump. +72 filler unknown (JSON keys? account link? server-signed?).
- Queued static: +72 provenance + filler recipe (JSON shapes vs object link
  vs crypto). Next live depends on it (extend sjson? link account? bypass?).
- WATCHPC proven as universal ladder tool (regs+backtrace, no slot, capped).

- WATCHPC proven as universal ladder tool (regs+backtrace, no slot, capped).

## 58. Four-subagent wave: lookup/loop/delivery/appender (2026-10-12, HYPOTHESES)
Status: read-only statics, jev-scored, byte-evidenced where marked PROVEN.
Nothing here ran live — every recipe needs live fill + binary verification.
Do NOT treat as fact; verify per item before building on it.

### 58a. Lookup 0x5E350 (INFERRED mechanics, PROVEN bytes)
- Semantics (bytes PROVEN): r0=map-owner Q, r1=key-holder Kptr, r2=out-ptr
  (caller stack), r3=flag (1=insert-or-find, 0=pure lookup). Stage1 0x5E4C8:
  RB-tree rooted at (r0)+0x20, key=**r1 double-deref, node key +16. Stage2:
  r3==1 -> find-or-create (0x5E92C); r3==0 (all ARM callers) -> pure search,
  returns node+20 or NULL. NOT a token/session store (type strings prove
  login/event-functor domain; token theory REFUTED 0.07).
- Null iff L1-null or L2-end; *out always written. r3=0 at all 7 ARM BL sites;
  flag-1 path DEAD in ARM (Thumb scan incomplete — caveat).
- Inputs needed: Q valid + root + matching node (producer UNKNOWN — zero
  proven writers; sessions/registry do NOT write it); Kptr valid + key match
  + *(P+16)!=0 (guard 0x5F9A0); r2=stack (free); r3=0 (immediate).
- Recipe (0.25, REVIEW): obtain live functor (never hardcode heap) ->
  satisfy guard 0x5F9A0 -> populate L1 (root *(Q+0x20), node +16==key) + L2
  (same key, +20 valid, [+12]==0) -> call order: boot -> 0x5CE50 ->
  RETSUCC sessions -> BL 0x5F96C / synthetic event with real functor.
  Producers to hunt: creator of $_1 functor (this+4/+8/+12/+16 writers),
  writer of P+0 key and P+16, writer of Q+0x20 root.

### 58b. Loop 0x5F96C invocation (PROVEN edges, INFERRED wiring)
- 0x5F96C = outer operator(), ARM, vtable 0x65E618 slot+24. Zero BL
  (ARM+Thumb exhaustive), 1 DATA hit (own slot). ONLY vtable dispatch.
- Virtual invoker 0x5E434 (slot +0x18): THE edge; its 7 BL callers all
  downstream/sibling (inside other outers), never upstream.
- Root poster chain (INFERRED async handoff, weakest link): 0x93A50 --BL-->
  0x5CE50 (sole BL caller, PROVEN) -> BL 0xDB658 -> clones/wraps ->
  enqueue 0x197D94 -> functors in jobs, never invoked synchronously.
  Second 0xDB658 caller (0x604C0) is loginWithUI-flow, EXCLUDED.
- Entry: r0 = outer __func* only (0x20B: +0=vtable 0x65E618, +4=proc,
  +8=owner, +12/+16=account/session, +20..=SSO). Gates: observer non-empty
  (0x5F9A0), lookup non-null (0x5F9CC), fired-flag (0x5F9D4), count
  (0x5FA24), per-element (0x5FA78).
- Recipe: forge outer (new 0x20, fields from live stash, NEVER hardcode) +
  BL 0x5F96C; or natural via 0x5CE50 (sterile in harness — completion parks).

### 58c. EventProcessor delivery (impossibility verdict 0.94)
- Poster: Lua binding loginRovioId -> handler 0x5D7C8 -> $_0/$_1 lambdas ->
  EventProcessor::enqueue chain (INFERRED decomp; entry bytes mismatch noted).
  Trigger = USER LOGIN ACTION in Lua, NOT job completion. Dead: handler never
  translates, Lua never calls, 0 hits everywhere.
- Queue: DefaultEventProcessor singleton (BSS 0x6DE278 INFERRED addr, live
  existence via class-sniff r0=0x66B7B8) + static event BSS 0x672DA8 + heap
  subscriber vector. NOT queue-B (distinct channel, do not conflate).
- Runner: nativeUpdate -> BL 0xC2140 -> walk 0xC45FC every update, count==0
  always, invoke never fires. Synthetic EVENTPOST = mechanism proven
  (INVOKE 27/27), content absent.
- Dispatch: outer builds __func/vtable 0x65E650 per element + 0x5E434;
  fail/success functors; loop 0x5E164 twin. All dead.
- VERDICT: no live recipe through the REAL processor exists in-harness.
  Needs live producer (Lua binding chain) that is itself dead. Direct
  0x5CE50 / 0x5FE70 / REDIR all run clean-sterile (log only). Named dead
  producer: loginRovioId -> 0x5D7C8 -> enqueue chain.

### 58d. Menu-node appender (PROVEN mechanism, INFERRED trigger)
- Node layout PROVEN: size 192 (0xC0, ctor 0x26305C), vtable VA 0x669710,
  [+120]=scene, [+180]=key1, [+188]=key2/stack, [+184]=0. +172/+152/+164/+168
  are VTABLE slots (0x250060 ldr+blx reads). Getters: +152 returns [+180],
  +164 returns [+84], +168 returns [+88], +172 returns address of [+108].
- Loop 0x250018: iterates +188 vector, per-node gates (helper==1, vt+152==r7
  key, vt+164 vs r9, vt+168 vs stack), miss path (0x2500E4-0x250158)
  allocates+constructs+pushes/grows — SOLE 0xC0 appender mechanism.
  Ctor 0x26305C has exactly 2 callers: 0x24FFF4 (factory helper) + 0x250124
  (loop). Builders zero BL (virtual-only); scene vt 0x669428 slots verified.
- Key 0x101929: no static occurrences, no heap hits — dynamic/computed.
  Candidate [+180] WEAK (0.13). manager 0x283B5C CANNOT append (single arg,
  no key regs; loading branch r1=3 only).
- Recipe (INFERRED, needs D1 to collapse this/key/caller/rect): preconds
  (clean registry, 1GB heap, mgr fields per §56, 1 vector entry, quiet
  workers; discover scene=[mgr+168], mgr=[scene+52], never hardcode) ->
  Step1: scene+0 virtual 0x250018(scene, r1=menu-id?, r2=string?, r3=int?,
  stack...) -> MISS push; Step2: scene+28 virtual 0x250AB8(subobj, key,
  rect count=4 valid) -> body -> writer r1=1 -> +164 flips as effect.
- Forge-suffices CONTRADICTED 0.96 (needs graph attach — proven by
  gate-bypass fault at writer).

### Cross-agent synthesis (my read, INFERRED)
- All four converge: producers dead at every level (Lua binding, map
  writers, posters, subscribers); structure fully mapped (exhaustive zero-BL
  scans); delivery mechanisms proven only synthetically.
- Sharp unknowns left: (a) guard 0x5F9A0 content (needs live functor —
  chicken-egg), (b) L1/L2 population (no writers — forge per W1 recipe),
  (c) loop-MISS menu args (r2-string? +84/+88? — D1 answers), (d) key source.
- Next: ONE live recon run (no new code): standard combo + WATCHPC on
  0x5F9C0/0xC4654/0x250124 if reachable, dump whatever live values exist
  for the forge recipes. Then fill per recipes (W1+W2 first: outer+trees+
  BL 0x5F96C) and verify binary. If functors fire -> downstream (events?
  builders? film?). D1 device capture stays queued backstop (needs live
  game + gadget reinstall + calm session).

## 59. Loginchain2 / nowait1 / plus912 verdicts (2026-10-12, journals the
   06:09-06:13 runs; no new runs needed)
- LOGINCHAIN2 (holder fix live): +32 holder=1a6515d0 wired to session
  10072230 on forged acct 1a651540 (PROVEN log lines). Initiation forged
  call hr=UD1 clean, spawned worker tid=13 (job 1a6517e0), tid=13 done
  retval=0 (PROVEN). r1=1 forged call hr=UD1 CLEAN — chain1's wild jump
  (pc=0x134) GONE (PROVEN). Holder fix removes the fault (PROVEN).
- LOGINCHAIN2 DELTA vs chain1 (PROVEN lines, mechanism INFERRED): forged
  acct+25 NOT set (pre25=0, post25=0; chain1 had pre25=1). Initiation's
  guard write went elsewhere with holder link. Natural D5994 fired inside
  the forged-call window (#3 r0=10072230 == session exact, frames
  C2240/57D10/57508/57228 — NOT the natural 93A50 chain of chain1 #2).
  Thread attribution OPEN (concurrent worker possible; WATCHPC is global).
  r1 interpretation: watchpc #3/#4 are D5994 hits (WATCHPC=d5994), NOT
  5ce50 hits — earlier "initiation r0" reading corrected (lr=5CED0 is
  stale-lr garbage per §46, never evidence).
- LOGINCHAIN2 STERILE (PROVEN): functors 5f96c/5fe70 = 0 exec; Login
  failed = 0; +164=2 held (diag164 obj=10002f98 succ=1); film stable
  non-white (shot.ppm==shot056.ppm md5 d7ae0c9862d1, white-frac 0.043,
  sky px e3c9f5) = LOADING held, no menu, no whiteout. D5994(r1=2)
  forged clean, post [+0x18]="ault" pre-existing bytes (PROVEN).
  Sesswatch exit flag=1 (PROVEN). Verdict: fault fixed, channel still
  dead — r1=1 runs but length/observer check downstream stalls silently.
- NOWAIT1 (HARNESS_NOCAPWAIT=1; hypothesis: uncapped cond waits keep pool
  alive -> completions): REFUTED. Workers STILL all done retval=0 (tid
  4,8,12,13 — same shape as capped). Functors 0, Login failed 0, heap
  top 1f426908 (1GB arena, no starvation). Pool death is NOT the wait
  cap. Keep NOCAPWAIT off default (no signal either way); worker
  one-shot lifecycle stands as mechanism (exit-after-job, no waiter).
- PLUS912 (PROBE5=c52ac; hypothesis: +912 vcall sometimes non-null):
  27 exec (every update), r0=00000000 ALL 27 (PROVEN). Always-NULL.
  Quit branch early-returns BY DESIGN every update; session/menu logic
  is NOT in this branch. Verdict: +912 CLOSED as menu path — stop
  spending probe slots on it (frees PROBE5 for recon targets).
- Slot budget after this: fill + raiser untouchable; PROBE5 now free
  (was c52ac). Recon targets per §58: 0x5F9C0 / 0xC4654 / 0x250124.

## 60. Natural initiation captured + heap drift + flake (2026-10-13)
- R1 RECON (WATCHPC=5ce50, no LOGINAT/RAWPOST): natural 0x5CE50 fires
  EXACTLY ONCE mid-run (line 8493/16432): #1 r0=10ad5060 r1=0
  lr=00093a54 (PROVEN). lr = exact return into sole BL caller 0x93A50
  (valid here — real BL edge, not stale). Frames: 2aee64/2af6dc/2ac2a8/
  2ca9a8/2fc04c/0cabc8/... (PROVEN). Fires amid worker DNS/job phase
  (t11 sscanf cloud.rovio.com, tid=12 created right after).
  Downstream STERILE: functors 0, Login failed 0, +164=2, flag=1
  (PROVEN). Natural initiation alone completes nothing either.
- RETRY (exact R1 command): LIVES, full run. Natural initiation fires
  again, same caller+frames, but r0=10ad52f0 (PROVEN). DRIFT +0x290
  run-to-run with IDENTICAL config (worker alloc interleaving).
  CONSEQUENCE: hardcoded heap addrs are out by construction (rule
  stands, now with a number); post-mortem HEXDUMP of a fixed addr is
  fragile. Forge must snapshot bytes AT HIT TIME. Code built:
  HARNESS_WATCHPCSNAP=<hexlen> in WatchPC (first hit wins, 64B cap,
  range-checked, rt_->loginat_acct_{_,len,snap}) — harness-only.
- R2 DEATH (WATCHPCSNAP-less config + HEXDUMP=10ad5060,60, 6-pair
  JOVR): instant death 0.08s at nativeConfig,
  `[jni] LOUD GetStringUTFChars unknown str 00000320 -- halting`
  (PROVEN line 182). JOVR-length theory KILLED by code reading (indexed
  `idx!=i`, no positional shift). Files-state theory KILLED (retry
  started from identical leftovers and lived).
- BISECT (exact R2 command): LIVES, full run (24s, watchpc #1 x2, no
  LOUD). R2 death = FLAKE, neither JOVR-6 nor HEXDUMP causal (PROVEN
  by contradiction). NEW FLAKE SHAPE (journal): instant config death
  via GetStringUTFChars(0x320), 1/4 this series. Early-heap
  nondeterminism can kill even call #1. Mitigation: instant deaths
  (<2s) get one free rerun before any theory work.
- NEXT: recon + WATCHPCSNAP=40 at 5ce50 (live account bytes) -> forge
  faithful account copy from snapshot (replaces zeros) + holder link
  -> LOGINAT rerun -> binary (functors/events/builders/film?).

## 61. Faithful-account verdict: necessary, not sufficient (2026-10-13,
   faithful1.log)
- F1 SETUP (PROVEN lines): natural initiation #1 r0=10ad5068 (lr=93a54,
  same chain); snap 64B from it; faithful copy @133a1518 (verbatim 64B:
  vtable 0x65E504, +25=0, +32=live link, all pointers same-run-valid);
  forged initiation #2 r0=133a1518 lr=halt (our call, PROVEN); r1=1 on
  the copy hr=UD1 clean, +25 0->0 (PROVEN).
- F1 STERILE (PROVEN): functors 5f96c/5fe70 = 0 exec; Login failed = 0;
  +164=2 held; film LOADING (white-frac 0.043, sky px e3c9f5, no
  whiteout, no menu); 4 workers done retval=0; flag=1 (sessions succ as
  usual). VERDICT: MISS — faithful bytes change nothing downstream.
- INVERSION (PROVEN lines, mechanism INFERRED): pre25=1 happened ONLY
  with zeros+direct-session (chain1, the variant that later faulted at
  r1=1); holder (chain2) and faithful (F1) both give pre25=0 with clean
  runs. The +25 write belongs to the faulty/early-out path, NOT to
  successful initiation. +25=1 is NOT a success marker — stop treating
  it as one (corrects §57's implicit reading).
- INFRA NOTE: RAWPOST did NOT fire this run (no rawpost lines) — stash
  fills only when WATCHPC=d5994 (WatchPC code), this run watched 5ce50.
  Not a game fact; D5994(r1=2)-clean stands from chain1/chain2.
- CONVERGENCE: initiation (natural AND faithful-forged) runs clean and
  parks; r1=1 runs clean and stalls; sessions succeeds and notifies no
  one. The dead piece is the COMPLETION CHANNEL (job tail
  notify/post/wake/loop-continuation), never the call — agrees with §48
  (channel-not-call) and §58c (impossibility 0.94). Calls are EXHAUSTED
  as a menu path; all three account shapes (zeros-direct, zeros-holder,
  faithful) tested live with binary zeros.
- NEXT: T2 static (read-only subagent): process 0x28DAF4 [+12] branch +
  entry fate (free/move/drop) + sessions-succ job tail (who posts
  completion? what condition? who consumes?) + minimal live recipe.
  Then loop-MISS menu args (W5 recipe attempt with forged graph args?);
  backstop D1 device capture (queued).

## 62. T2 job-tail + E1 enqueue attribution: sessions never posts
   (2026-10-13, notifylive.log + enqueuewho.log)
- T2 STATIC (read-only subagent, jev-scored): success runner 0xD5DA4
  (gate [job+60]==0, virtual slot+28 = handler 0xD6C78, lr=D5DE4) ->
  flag write 0xD70FC (mov#1/strb+120, PROVEN bytes) -> tail
  (0x6141C/new/0x1A96EC, NO bl to 28DD04/28ED64/28DB9C/28DBBC on success
  path — PROVEN absence) -> 0x1A96EC posts ONLY to Identity event array
  (append under mutex, NO pipe write, NO counter inc, NO cond-signal —
  PROVEN). Dead link 0.91: channel mismatch (array vs pipe/queue).
  [+12] branch UNRESOLVED (3 hypotheses ~0.35-0.5; do not gate on it).
  Dead producer named: 0x2A1A34 (zero static callers) + worker->main
  pipe-notify. Recipe: direct 0xD5DA4 with live objects, or accept
  impossibility if array stays empty (needs HTTP-200 response object +
  subscriber table + 0x2A1A34 poster — all dead).
- T2a NOTIFYLIVE (RETSUCC combo, WATCHPC=28ed64): notify FIRES 2x
  (#1 r0=1028e3e8 @line 1114 post-registry-save; #2 r0=12f75198 @line
  8481 amid DNS phase; both lr=28dd6c = called FROM enqueue, as T2
  decoded). Sessions handler ran 2x (10072230 via D5DE4 @8354/@8561),
  flag=1, +164=2, film LOADING, functors 0, Login failed 0. So notify
  channel ALIVE but sessions-adjacent zeros everywhere.
- E1 ENQUEUEWHO (WATCHPC=28dc00 trampoline + PROBE4=28dd04): 2 enqueues
  (caps NOT hit: 2<4 WATCHPC, small probe totals — observation
  complete). Both via 0x291A5C (lr=291a74; BL scan: 5 callers incl.
  live 0x198990). #1 @line 1010 (early boot, pre-sessions) = non-
  sessions job, PROVEN by timeline. #2 @line 8507 carries a STACK-BUILT
  entry (28DD04 r1=fe7ff10c) = synchronous inline post, NOT a heap
  job-completion record (BET2's well-formed {MAGIC...} shape absent).
  Sessions runs (@8354/@8561, obj 10071FA8) have NO adjacent heap-
  record enqueue. VERDICT E1: sessions completions NEVER enqueue —
  T2 dead-link CONFIRMED live in RETSUCC config. The 2 observed
  enqueues belong to other flows (early-boot job + main-side inline
  post); poster helper 0x291A5C shared, outer roots differ (1a1c1c vs
  17ea58). r1=5fc18471 constant across notify hits = stale (side-show,
  not evidence).
- CONSEQUENCE: T2's direct-0xD5DA4 recipe half is REDUNDANT (natural
  sessions-succ IS that path running live every run — flag=1 proves
  it); its impossibility condition is ACCEPTED (array drains nowhere,
  subscriber count stays 0, worker->main wakes stay 0). Stop spending
  runs on the sessions completion channel — closed from both ends
  (static absence + live silence).
- NEXT: loop-MISS menu args (funnel #2; W5 recipe: scene+0 virtual
  with menu-id + Step2 builder 0x250AB8) — check existing loopcall
  infra first. Then D1 device capture (needs live game + gadget
  reinstall + calm session — user-gated).

## 63. D1 BUILDER-CAPTURE GOLD (2026-10-12 eve, gadget repack + hook_d1)
- SETUP (PROVEN): gadget APK (engine md5 9c21a987 identical) reinstalled
  over wiped original (user-approved); sandbox-only, /sdcard untouched.
  Gadget hangs till connect (user-warned) + connect-timeout kills the
  process (user-warned): must force-stop before start + attach fast.
  Base 0x147F4000 deterministic 3rd boot (needle nativeConfig@0xC1550).
  Hook method VALID (hookcold2.log 01:15 captured same VA before).
- SELF-CRITIQUE (burned, journaled so it sticks): mid-run "0 hits"
  checks were (a) grepping the OLD session file (d1_boot.log vs
  d1_boot2.log) and (b) tail-truncating the live file. Session 1 = 0
  hits TRUTHFULLY (stale pre-booted process, builders fired pre-attach).
  Session 2 (true cold boot) = 6/6 hits ~11s post-attach, identical.
  RULE: always grep the CURRENT session file + full hit pattern.
- D1 HITS x6 (PROVEN, identical, ~100ms burst): builder 0x250AB8,
  this=0x9B262000 (SAME addr as hookcold2 01:15 + stable across boots),
  [this]=vtable file-VA 0x669404 (code-verified: 0x14E5D404-0x147F4000;
  = GL_Context vtable per §40-correction — NOT scene 0x669428!),
  r1=0x101929 (menu key, device-confirmed 2nd time), r2=0, r3=0
  (recipe's r2=string/r3=int guess REFUTED — both zero live).
  Stack args LOST (this.sp.readPointer FAIL x6 — Frida/ARM quirk, open).
  bt (ACCURATE, identical x6, file VAs): 7ee3c a427c a41d0 a4108 a4058
  a3fb0 a3f14 39720c 39d3a8 3977b0 396b8c 397a60 39b744 279b44 85b894
  85b1f4 (return addrs; decode pending).
- MENU WITHOUT 250AB8? NO — resolved: builders fired +11s = menu
  construction itself (boot is 10s; menu+popup visible right after).
  Session-1 zero was stale-process artifact, not an alternate path.
- CORRECTIONS: (1) builder-this vtable = 0x669404 (GL_Context family),
  NOT 0x669428 (builder2 forge used the wrong vtable — explains void
  build). (2) r0 stable 0x9B262000 across boots = shared build context
  (surface-ish singleton?), NOT per-node. (3) r2=r3=0 live.
- DEVICE MENU STATE (screenshots, PROVEN): boot->menu works on gadget
  build; Bird-Coins popup first run; OK tap (900,745 shot-coords==input
  coords) dismisses -> full menu (PLAY disc + ToonsTV + gear, = §54
  reference exactly). Game+frida stable through all of it (no crashes).
- NEXT: decode bt driver chain (direct caller 0x7EE3C fn? 0xA4xxx
  cluster? 0x39xxxx cluster? root 0x85Bxxx?) via distv/BL-scan ->
  natural driver recipe for harness (who calls builders with menu key
  + this=GL_Context-ish + r2=r3=0). Then W5 forge with CORRECT vtable
  (0x669404) + live arg shape.

## 64. Builder driver decoded: Lua drawRect, r1=color NOT key (2026-10-13,
   read-only static subagent, byte-proven where marked)
- DISPATCH (PROVEN): func 0x7ECB8 contains `blx r6 @0x7EE38` (ret
  0x7EE3C = our bt[0]); r6 = [vtable+64] where vtable = [[r4+88]+0];
  vtable VA 0x669404 slot+64 = 0x250AB8 (python bytes); typeinfo =
  `gr::gles2::GL_Context` (PROVEN). So builder-this = live GL_Context
  instance fetched from [r4+88] (r4 = Lua-bound scene obj). Zero direct
  BL to any builder/loop (PROVEN exhaustive) — virtual-only stands.
- r1=0x101929 = PACKED COLOR (PROVEN bytes: float vmov→×255→ORR chain
  0x7EE10-34; provenance Lua floats via 0x2791A4 getters). "Menu node
  id" label REFUTED (bytes favor color 0.59-uncertain but mechanistically
  exact). CORRECTIONS with back-pointers: §55/56 "menu node id 0x101929
  / dynamic graph node id / zero heap hits" = MISLABELED (never stored,
  computed per-frame from floats — zero-hits EXPLAINED, hunts for it as
  id were void by construction). §58d/W5 Step1 (loop with 0x101929 as
  node key) = SUSPECT (loop key arg never observed on device; do not
  run W5 as written).
- CHAIN (PROVEN edges): Lua C-funcs 0xA3EB4→0xA3F40→…→0xA41FC
  (`drawRect`, registered name bytes @0x5C161F PROVEN, zero BL callers =
  Lua-only) → invoked via blx through Lua VM invokers (0x396B44/
  0x396FA8 bodies PROVEN) ← LuaState pcall 0x279ACC (PROVEN) ← root
  0x85B894/0x85B1F4 = OUT-OF-RANGE (file ends 0x6722F4; ACCURATE
  backtracer top-2 frames are heap garbage — STALE, discard; do not
  chase 0x85Bxxx). Best root INFERRED (0.67): nativeUpdate frame →
  Lua menu script → pcall → drawRect ×6 rects in one frame.
- 6 HITS = 6 RECTS, not 6 pages (identical args incl. color; ~100ms
  burst = one frame drawing menu rects). Builder = per-rect draw call.
- REPLAY RECIPE (INFERRED, jev 0.91/0.86): (a) natural: boot to menu-
  construction → live GL_Context (heap-scan vtable 0x669404, never
  hardcode; 0x9B262000-family) → Lua drawRect 9 args through pcall;
  (b) bypass: direct slot+64 call (liveThis, 0x101929, 0, 0, ...).
- REFINED FORGE RULE: [this]=0x669404 means r0 IS a GL_Context (vtable
  READ, not pasted) — forge needs live object layout, vtable-swapping
  zeros (builder2 with 0x669428 AND any 0x669404-paste) insufficient by
  construction. §63 correction-(1) refined accordingly.
- NEXT (free first): grep existing logs for Lua-VM translation evidence
  (0x279ACC/0x39B6CC/0x7ECB8/0xA3EB4 in cover1 PCTRACE + probe logs) —
  absence of translation = Lua never runs in-harness. Then: L1 (what
  pumps Lua? which update-path call enters pcall?) or direct slot+64
  forge with live GL_Context (HEXFINDHEX for vtable 0x669404).

## 65. L1a: Lua pump alive, drawRect dead (2026-10-13, luapump.log)
- pcall 0x279ACC fires 4x (WATCHPC cap): r0=0x100038B8 EVERY time (same
  LuaState; NOTE = snapshot-account +16 value — account links LuaState,
  cross-fact), r1=0, lr=0x27A53C (same caller). Frames differ per hit
  (#4 chain via 0xC1B5C/0xC3C70 = update/init region → updates DO pump
  Lua). Cover1 PCTRACE+ (free mining): 0x27ECB8 x2, 0x279ACC x1,
  0x2791A4 x1 translate — Lua machinery runs in-harness (PROVEN).
- drawRect 0xA3EB4 exec = 0; dispatcher 0x7ECB8 exec = 0 (PROVEN,
  probe-#N; caps not hit). Menu scripts load (full ui/ list incl.
  rovioaccount.lua, popup.lua, menuButton.lua) but their drawRect burst
  never fires. VERDICT: pump alive (4 pcalls), menu-draw dead.
- RULE (user-steered, standing): device-via-Frida beats hypothesis
  brute-force — mine the working game first, forge second.
- NEXT D1b (device, game alive at menu, NO restart): hook float getter
  0x2791A4 (r1=arg idx + retval float bits, cap ~60 = one 6-rect burst,
  then detach) + drawRect 0xA3EB4 counter (cap 10) + pcall first-3 with
  bt. Yields live rect args + burst shape + pump identity. Then harness
  mirror (drawRect chain or direct slot+64 with captured args + live
  GL_Context).

## 66. D1b: live rect args + Java-driven Lua pump (2026-10-13, hook_d1b)
- IDLE MENU SILENT (PROVEN, ~105s, 0/0/0): no getter/drawRect/pcall on
  the idle menu — all Lua/draw activity is construction-burst only.
  (Session-1 lesson: hooks must catch boot→menu fresh.)
- BURST (PROVEN): drawRect 0xA3EB4 x10 (cap) #1 lone + #2-10 in 141ms;
  60 fargs (cap → detached). Rect float sets (bits→float, code-verified:
  2.025 44.0 1.0 1066.0 488.0 19.0 21.0 1.174 73.0 74.0 1584.0 842.0
  116.0 30.0 ...). Order per rect: 1,2,3,4,5,6,7,8 then 2,3 again
  (nested re-read? mapping needs 0xA3EB4 body decode — open, static).
  Steady rect shape: [1:0, 2:0, 3:C, 4:C, 5:0, 6:X, 7:Y, 8:1.0] +
  [2:W, 3:H] (C=color-ish small floats, X/Y/W/H geometry-ish).
- PUMP IDENTITY (PROVEN, biggest find): pcall#1/#3 bt =
  0x6CDAC ← 0x878F8 ← 0x65AFEC ← 0x695BC ← 0xBC124 ← libart
  JNI trampoline. #2 (builder-bt tail!): 0x6B894 ← 0x6B1F4 ← 0xBC124
  ← libart. So a JAVA→native call (fn at 0xBC0AC, prologue-verified
  e92d48f0) drives Lua pcall → drawRect → builders. CORRECTION (my
  transcription error, subagent rightfully flagged OOB): bt addrs carry
  full 0x14 prefix (0x1485B894 etc.); file VAs = 0x6B894/0x6B1F4/0xBC124
  (code-verified subtraction, not mental). RULE: transcribe device
  addresses byte-exact, never shorten.
- 0xBC0AC is NOT a static Java_ export (63 exports scanned, none in
  [0xB9000,0xBF000)) → dynamically registered native. Smali diff: 76
  natives vs 63 exports = 15 callback-natives without exports
  (VideoPlayer/Location/Camera/Share/RemoteNotifications) = the
  registration family. fn#215 numbering VERIFIED vs NDK r27 jni.h
  (NDK idx 211 + 4 reserved = ours 215 — numbering stands); case-215
  logger built, but OnLoad never calls it in-harness (PROVEN silence) —
  registration happens on a path that doesn't run here (another dead
  producer of the same class).
- DEVICE LOGCAT TIMING (rio_boot2.log, read-only, PROVEN lines):
  Login failed x2 @01:02:19 → WebViewFactory loading @01:02:28 →
  "Main menu entered" @01:02:29 (+retry login-fail same second). Menu
  builds as WebView loads — pump trigger sits in that window (WebView
  bridge? login retry? coincidental ads init? — open, not evidence).
- NEXT: device hook 0xBC0AC onEnter (r0/r1/r2/r3 + sp + short bt) on a
  FRESH boot (builders at +11s) → live arg shape → harness mirror: BL
  0xBC0AC with faithful JNI args (new trigger!) → watch
  drawRect/builders/film. If 0xBC0AC silent on fresh boot → next
  candidate from the 15 (or the WebView angle).

## 67. BC0AC fires 4x at +11s + 18-caller table + pctrace zeros (2026-10-13)
- BC0AC DEVICE (fresh boot, hook_bc, PROVEN): 4 hits (#1 lone, #2-4 in
  70ms), args IDENTICAL: r0=0x9b21c2b8 r1=0x2ca623b0 r2=0x15049274
  r3=0x15049240. Timing +11s post-attach = construction window. bt
  (identical x4): BC0AC <- 0xB004C <- 0xAFFA8 <- 0x39720C <- 0x39D3A8
  <- ... <- 0x6CDAC <- 0x878F8 <- 0x65AFEC <- 0x695BC <- 0xBC124 <-
  libart JNI trampoline. So 0xBC0AC serves BOTH a native caller
  (0xB004C) AND Java (registered native; libart frame direct). mem-read
  (sp/[r0]) FAILED x4 (ART/protected addrs — arg shapes stay opaque).
  Earlier "0 hits" was early-check race (attach lag 10-40s); rule: never
  verdict zero before session end.
- 18-CALLER TABLE (read-only subagent, PROVEN bytes): all 18 pcall
  wrappers named (update/input/physics/ads/lifecycle/…); menu-driver
  pick WEAK tie (0x68D3C/update 0.32, nothing >0.5 — honest, no force).
  0xBC0AC has NO ARM BL to pcall/18 (extent scan) — path indirect.
  0x85Bxxx OOB = MY transcription cut (0x14 prefix dropped), subagent
  correct to flag; device addrs transcribed byte-exact from now on.
- PCTRACEFULL (full-range, PROVEN zeros): NONE of the 18 wrapper starts
  translate in-harness (all 0/500k); pcall body 0x279ACC = 1 (via
  0x27A4C8 tail-path of the 4 boot loads); drawRect/dispatcher/BC0AC =
  0. The whole direct-pcall layer is dead here; menu driver must arrive
  via BC0AC-indirect or an untranslated wrapper.
- NEXT (computer): static decode 0xBC0AC body (arg use: r1 jobject?
  r2/r3 ints/strings?) + verify native caller 0xB004C (BL word?) ->
  harness mirror BL 0xBC0AC with faithful args (new trigger J1) ->
  binary (drawRect/builders/film?) vs fault (pc+addr = requirement).

## 68. J1: consumer runs deep, faults on null content (2026-10-13, j1.log)
- J1 (PROVEN lines): forged C={0x251260 bx-lr,0} + D={0,0,0} + B=0 ->
  BL 0xBC158 -> blx C[0] returned fine -> continuation via
  0x27EEB0-chain -> 0x27F1BC struct-copy with r0(dest)=0: BAD writes
  @8,@C,@10,@14 (pc-lag shows 0x27F228), hr=UD2 abnormal, r0=stack.
  0xBC158 does NOT call 0x27F1BC directly (its BLs -> 0x27EE8C/0x27EEB0;
  0x27F1BC has 2 callers: 0x27B59C/0x27EE70 — chain via 0x27EEB0,
  PROVEN words). Downstream probes (drawRect/dispatcher/builder): 0.
  +164=2, film LOADING (continuity).
- VERDICT J1 (mechanism ALIVE, content DEAD): consumer machinery runs
  (BC158 -> 27EEB0 -> … -> 27F1BC attempted 4 writes) — the fault is
  null CONTENT (B=0/D-zero -> lookup yields null dest). FORGE REQUIRES
  live B/C/D content (poster-supplied). Note: device C/D live at heap
  0x15049xxx (inside our heap-range shape!) with Δ(C-D)=0x34=52 — same
  52B Δ as account+32->session (0x100721F4->0x10072228): possibly one
  container class (event record? session-link?), INFERRED, open.
- NEXT D1c (device, fresh boot): hook 0xBC0AC + 0xBC158 entries, read
  [r2+0/4/8] + [r3+0/4/8] as WORDS via this.context (not this.sp —
  this.sp.readPointer failed x10 total, use this.context like hook16's
  working r10 read), cap 4, short bt. Yields live C[0] as file VA
  (directly reusable!) + content shape -> J2 mirror with live funptr +
  live-shaped record. If words unreadable (ART-guarded) -> static
  poster hunt instead (who writes {funptr,tagged}+3 words 52B apart?).

## 69. D1c gold: live record + delete-dispatch + J1-safety (2026-10-13)
- LIVE RECORD (PROVEN, this.context reads work): BC0AC#1/#2 (fresh boot,
  +11s): r0=0x9b21c258 r1=0x9b205af0, C=[0x148aea58, 0x0],
  D=[0x11, 0xd, 0x3950e260] then [0x21, 0x1f, 0x39410600] (D varies per
  event). C[0] file VA = 0xBEA58 (code-verified subtraction). C[1]=0
  (even/direct path). r0 discarded by body (mov r0,r1) — B=r1 flows to
  functor r0.
- 0xBEA58 = TAGGED-VALUE dispatcher (PROVEN distv): tag/ptr pairs from
  stack buf ([sp+40]/[sp+48], [sp+16]/[sp+24], [sp+28]/[sp+36]...),
  tst #1/beq skips, all 5-6 callees -> PLT 0x5369C/0x536A8 = idx4/5 =
  _ZdlPv (operator delete) + __stack_chk_fail (code-verified PLT math).
  So C is a cleanup/ack functor (frees tagged payloads), NOT the draw
  path (draw flows via pcall<-Lua separately). Routing refined: Java
  entry region -> native chain -> BC0AC(r0,B,C,D) -> BC158 (copies D,
  zeroes it, blx C[0]) -> BEA58 deletes tagged words.
- J1-SAFETY (PROVEN by code): unknown imports incl. _ZdlPv fall to
  generic [stub]->0 (logs throttled + Ret 0); GuestFree is no-op.
  J2 (C={0xBEA58,0} real funptr) is host-safe: deletes no-op, faults
  (if any) name requirements. No host corruption possible.
- GHIDRA-VA RULE (user asked; standing, journaled): GhidraVA-0x10000 =
  fileVA uniformly (code AND data incl. DAT_). Old "FUN_==file" claim
  burned hours (0xD45FC slip, wrong new(0x2C) record); agents caught 2
  more slips since. Every load-bearing addr verified by prologue bytes
  (distv) + python LE — decomp hypothesis-only. We do NOT have that
  problem anymore (it is a checked rule, §44 VA RULE).
- NEXT J2 (built: HARNESS_BC158LIVE=1): same BL 0xBC158 with live
  record C={0xBEA58,0} D={0x11,0xd,0} B=0 -> binary (runs the real
  delete-dispatch clean? faults where=next requirement? downstream?).

## 70. J2: same fault; consumer chain fully dead in-harness (2026-10-13)
- J2 (PROVEN): live C[0]=0xBEA58 runs SAFE (deletes no-op via [stub]->0,
  zero crash) -> identical fault (BAD @8..@14, pc 0x27F228, hr=UD2).
  Fault is DOWNSTREAM of C[0] (27EEB0-chain dest null), independent of
  C content. VERDICT J2: mechanics alive, needs live B/D content.
- TRANSLATION MAP (pctracefull, PROVEN zeros): b004c=0, bc048=0,
  bc158=0, bea58=0, 27eeb0=0, 27ee70=0, bc124=0; only 27f1bc=1 (via the
  OTHER caller 0x27B59C — unrelated flow) and 27ecb8=2 (Lua cover).
  The ENTIRE event-consumer side never translates in-harness;
  poster+consumer both dead. J-ladder needs live B/D (deep poster
  chain) — expensive; climb the DRIVER side instead (who calls AFFDC?).
- 0xB004C lives in fn 0xAFFDC (prologue scan); sole ARM BL->BC0AC is
  0xBC048 (same region — Java-entry family's own wiring).
- NEXT: driver ladder UP from 0xAFFDC (BL callers? do they translate?)
  — one static scan + pctrace grep per rung (all free) to the highest
  dead link, then forge/call THAT.

## 71. drawRect-is-not-Lua; producer-timeline plan (2026-10-13)
- KILL (PROVEN, free): all 649 APK lua files contain ZERO drawRect
  (case-insensitive). No script calls it — the "Lua C-func" label is
  WRONG (registration real: 0x28EBFC code + rodata drawRect@0x5C161F,
  prologue 0xA3EB4 real — a live-but-script-unreferenced binding, or
  misread decomp). 0xA4xxx = NATIVE scene-graph draw path (rect
  primitive via GL_Context slot+64; 0x2791A4 = generic float reader;
  0x279ACC = generic protected-call). Lua EXISTS separately (scripts
  load, 4 boot pcalls) but is NOT the menu-draw driver.
- CONSEQUENCE: menu driver = scene-graph with menu NODES (device has
  them; we have 1 loading node). Appender still loop-MISS only; node
  key source still unknown; r1=color correction stands (draw param,
  not id). Every framing converges on the same missing live producer
  (completion -> subscribers -> nodes -> draws).
- NEXT D1e (device, ONE fresh boot, all sparse): loginFinished (2 hits)
  + drawRect 0xA3EB4 (cap 10) + builder 0x250AB8 (cap 6, this/r1) +
  gate164-change-only, ALL with timestamps. Yields the PRODUCER
  TIMELINE (what fires between loginFinished and the +11s burst?) —
  sequences the missing link without guessing it. No arg-heavy hooks.

## 72. Producer timeline + gate-hook verdict (2026-10-13, d1f3.log)
- TIMELINE (PROVEN, fresh boot, no gate hook, session survived to
  detached-ok): loginFinished(ctx=0x152934d0, flag=0) t=…649205 ->
  10.2s gap -> drawRect#1+builder#1 (this=0x9b262000, r1=0x101929) ->
  alternating builders r1=0x101929/0x7F000000 (odd/even; 0x7F000000 =
  ARGB half-black — second draw color!) x6 in ~30ms -> drawRect#7,#8
  -> loginFinished(SAME ctx, flag=0) t=…659491 (retry, DURING burst)
  -> drawRect#9,#10 (cap) -> clean session end. Matches §39 gold
  (login pair same ctx) + D1 burst shape.
- GATE VERDICT: full 200s session, zero hang, zero crash WITHOUT the
  gate-hotpoll hook (d1e WITH gate died at builder#1 twice... though
  d1e-death also fits timeout-race; gate-guilty INFERRED 0.7, not
  proven — keep gate hooks OFF by default regardless: cost/benefit).
- CORRECTION: builder-this 0x9b262000 (this boot) vs 0x9b258000 (d1e
  boot) — same arena, NOT stable. Never hardcode; §63 "stable" weakened
  to "same-arena". (Slot base also varies: 0x14804000 3rd slot —
  slot-verify list extended, full-scan fallback KEPT (this attach
  needed it and survived: scan-stall-kill theory weakened too).)
- PRODUCER BOUNDED: 10.2s gap login#1 -> burst; retry-login#2 lands
  DURING burst (+10.3s). Producer ≈ login-retry job path (refresh/
  fail-cleanup -> drawRect burst), NOT a timer (burst tied to retry
  completion, not wall clock).
- COVERAGE GAP FOUND (self-critique): fail-path harness runs NEVER had
  builder/drawRect probes (all builder watches were in RETSUCC/succ
  configs!). Device burst follows login-FAIL — our fail configs
  (airplane1 flag=0, relthrow, failpath3) may well fire builders
  unobserved. NEXT RUN (cheap, decisive): fail config (servers+
  sendfail, NO RETSUCC) + PROBE4=a3eb4 + PROBE5=7ecb8 + WATCHPC=250ab8
  -> binary: fail path fires drawRect/builders in-harness? If YES ->
  menu via fail completion (functor-free?); if NO -> producer still out.

## 73. RETFAIL builder-watch: fail-throw path silent (2026-10-13)
- RETFAIL RUN (PROVEN): 63/63 calls, sessions handler 2x, exit flag=1
  (fresh-registry free path, no RETSUCC), but 2 worker unwind spirals
  (513 deep @0x45A778) — RETFAIL's lr-gate didn't cover all raiser
  paths (time/apdrive throws escaped). drawRect=0, builder=0, Login
  failed=0, DIAG164 lines ABSENT (0x — run ended abnormally shaped).
- VERDICT: throw-flavored fail does NOT fire builders in-harness
  (workers die pre-completion; catch never lands — unwinder CLOSED).
  Remaining fail shape untested with builder probes: airplane1 CLEAN
  flag-0 (refresh fall-through, NO throw, NO spiral) = closest to
  device (flag=0, alive). NEXT: files17 token-registry + pure NONET,
  no RETSUCC, builder/drawRect probes.
- SHIP SCOPE (user asked; standing): YES, this affects the APK too.
  Device-original reaches menu via throw->catch->loginFinished(0)->
  burst (logcat-proven); success branch also needs the completion
  channel (functors never materialize). Both require (a) working
  host unwinder (R7=0 open, MCSM ships libunwind-style — precedent),
  (b) completion channel job-tail, (c) real Java/timers (free on
  device). Harness pain = building these; device = proving them.
  Harness-only crutches (RETSUCC/fills/pokes) must be replaced by real
  mechanisms for ship (CRUTCH LEDGER). Unwinder + channel are
  ship-blockers, NOT harness-only.
- APK-NOW (user asked; verdict NO): harness-only pain (mocks/caps/
  pacing) would vanish on device, but load-bearing walls are
  mechanism (Dynarmic-context R7, channel, dynamic natives) and would
  reproduce as LOADING-without-menu — predictable without building.
  Iteration 1min vs 10+min per hypothesis. Package at first
  in-harness menu (even crutched) as logo-on-device milestone.

## 74. Airplane-flag0: all fail shapes exhausted (2026-10-13)
- AIRFLAG (files17 496B token-registry, pure NONET, no RETSUCC,
  fixture restored after): exit flag=0 (refresh fall-through, PROVEN),
  63 calls, no spirals. drawRect=0, builder=0, Login failed=0.
  VERDICT: clean flag-0 ALSO silent. All three in-harness shapes now
  binary-zero on builders: succ/flag=1 (many runs), clean-fail/flag=0
  (this run), throw-fail/worker-death (retfailbuild). The ONLY untested
  combination = fail + CATCH (loginFinished(0) with live ctx ->
  downstream) = unwinder R7 + job tail. Convergence complete.
- NEXT (the only door left): unwinder root-cause workstream (VRS[R7]
  init under Dynarmic; MCSM custom-_Unwind_Context precedent; §18 +
  §43 + §51 notes). Scope first (Dynarmic headers docs-first), then
  build. No more trigger runs until R7 moves — all trigger shapes are
  measured.

## 84. Online-APK packaging kickoff (2026-10-13, user-steered)
- DECISION (standing): online-first (real Java gives triggers free, no
  catch needed); offline (R7) after as upgrade. Packaging now, menu
  path via success branch.
- P0 DONE: work/apk_prod = apktool decode of original (58MB APK).
- P1 DONE: loader keeps defined-symbols map (GuestLookup for Java_*).
- P2/P3 DONE: host/tools/gen_apphost.py -> 63 JNI forwarders,
  verified 63/63 vs exports, no overloads; returns from smali
  (V/I/Z/B/J/F/L), AAPCS incl. jlong-align + stack spill, runtime walk
  via va_list (J raw, F via double promotion, I ints, L pointers).
- P2 host lib DONE: abhost.so (arm64, JNI_OnLoad + 63 + OnLoad):
  mini-zip STORED extract of guest from own base.apk, per-thread Jit
  + 2MB stack + real AttachCurrentThread, REAL JNIEnv* passthrough
  (indices match NDK order), files->app dir, logs to files/apphost.log
  (+logcat), init_array minus parked 0x5885C, guest OnLoad run.
  Android fixes along the way: fmt include, narrowing cast,
  Screenshot stub, ThreadEnter accessor (anon-namespace locals).
- NEXT P5: APK assembly (Gen.library_name -> "abhost"; guest .so ->
  assets/guestlib STORED; drop adcolony; host -> lib/arm64-v8a;
  build+sign; install) -> logcat milestone boot->logo ONLINE.

## 85. First product APK boot: host lives, JavaVM bug found (2026-10-13)
- BOOT (PROVEN logcat/file): extract 6759156B ✓, inits 54/54 ✓,
  guest OnLoad ✓, Java drives config/init/update (screen rotates!).
  Product pipeline works end-to-end (pack/sign/install/launch).
- BUG (PROVEN): passed real JNIEnv* (64-bit) as JavaVM* to guest
  OnLoad -> slot init reads ART garbage -> BAD read @0x2FA344 (same
  site as PC §5, different garbage) + updates end UD2 (r0=8), black
  screen. FIX: mock-VM handle (rt.Jvm()) for guest OnLoad (32-bit
  safe); real env enters only via per-call delegation.
- DESIGN (standing): guest keeps OUR 32-bit JNI table; thunks delegate
  to real env with guest-side copies (strings/arrays) or cookies
  (classes/methods/refs); RegisterNatives goes REAL (dynamic pump
  must bind!); tuned mocks shrink to era-needs (API21 etc. only if
  real breaks). Implemented now: g_real_env_hook + 168/169 delegate
  (Android-gated; PC identical by preprocessor).
- NEXT: repack + reinstall + launch -> config BAD gone? updates clean?

## 86. First APK boot forensics: JavaVM fixed, files gap found (2026-10-13)
- BOOT (PROVEN): extract/inits/OnLoad/Java-drive all live on device;
  screen rotates (GLSurfaceView up). Black screen: updates end UD2.
- FAULTS (PROVEN log): (1) config BAD read @ART (0xDEE36180) pc=0xC1598
  (ran further than PC §5 site — slot init fixed, next wall is
  getActivity-mock vs real use); (2) update#1 HALT on bare
  "highscores.lua" (files bridge had no CWD-relative mapping; cascade
  kills all later updates at 0x39B1E8 NULL writes). DeviceID mock
  fired fine; Attach on 2 threads fine (tid 1000/1001).
- FIX (this commit): bare relative non-data names -> files dir
  (fopen-fails-graceful like device; data/* untouched — PC proves
  asset API serves it). Strictly more permissive; PC inert unless a
  run relied on the halt (none observed).
- NEXT: repack/reinstall/launch -> update#1 passes? config BAD persists
  (then: Call*-delegation for real Activity)?

## 89. Live-this builder: clean no-op; gate needs real rect (2026-10-13)
- BLIVE (PROVEN): live GL_Context this=0x10002CD8 (heap scan, == surf
  landmark) + device args (r1=0x101929, r2=r3=0, stack zeros) ->
  hr=UD1 CLEAN r0=0, +164=2 always, film LOADING (white 0.043). NO
  writer fault (unlike forged) — but also no content: frustum gate
  SKIPS on zero rect (consistent with builder3/4 PCTRACE).
- ABORT SIDE-FIND (builderlive1/2, PROVEN twice, baselines 0):
  worker abort() (r0=heap, lr=0x38F38C pool) AFTER cond-timeout in
  DNS/URL worker, BEFORE builder call (line 8763 << 15557). NOT the
  heap scan (bound 0.0s rerun still aborts) and NOT the builder call.
  Correlates with BUILDER* env presence... or flake (2/2 vs 0/N).
  OPEN, low priority (builder verdict unaffected: call ran clean after).
- GATE STATICS (myself, PROVEN bytes): gate fn @0x250704 (not 0x25070C);
  one gate-BL per builder (0x2505B0 in 0x250200, 0x250C7C in 0x250AB8,
  0x251158 in 0x250E00). In-builder: rect=sp+100 (stores s6/s0/r0/
  s14/s2/r0 at +124..144 adjacent), count=r1, `cmp r0,#1; bne SKIP`.
  Gate body: VFP clamp loop (pool consts + per-item [r0-4]/[r0],
  stride 12, r1 iters) + final `-1.0f` check. r1-at-call + rect layout
  + return-1 condition = queued static (subagent).
- NEXT: subagent decodes builder r0/r1 at gate + return semantics ->
  forge valid rect+count -> BLIVE rerun -> binary (body->writer->
  flip/content?) vs fault (requirement).

## 90. Gate passes, writer fires, flip transient (2026-10-13, builderxy1)
- RECIPE PROVEN LIVE: direct BL 0x250AB8(live GL_Context 0x10002CD8,
  r1=0x101929, r2=r3=0, stack X=488,Y=44) -> gate passes -> body runs
  -> writer 0x283A3C(mgr,1) FIRES (probe #23 r0=mgr r1=1 lr=0x250DB4,
  exact builder5 site) with NO fault (live this fixes builder5's
  writer-fault!). +164 flipped 2->1 once, then loading re-asserted
  (->2); film LOADING (white window missed or never).
- NOT YET: nodes (scene188 stays 1 loading node; builders don't
  append — only loop-MISS does, and it never fires naturally).
  Content needs NODES, not just flips.
- NEXT: (a) 6x burst alternating colors (device shape) on PC (cheap,
  binary: nodes/flip-hold/film?); (b) APK synthesis with live graph
  (real attach target — PC void by construction).

## 88. Product hits the unwinder wall (2026-10-13, device APK)
- FILES ALIAS WORKED (no unmapped lines; seeds open). Config BAD gone
  (no BAD @config). First update: hr=UD2 r0=0x45A778 = THROW+spiral
  (real network -> dead API/TLS -> HttpRequestException -> R7 wall),
  then 0x39B1E8 NULL-write cascade (r0=8), black screen. Same shape as
  harness/device-headless predicts NOTHING (all three agree).
- VERDICT: unwinder catch is UNAVOIDABLE for any menu (API dead online
  and offline -> always throws -> must catch -> loginFinished(0) ->
  burst). The R7 workstream is THE product critical path (PC lab, fix
  transfers everywhere). No more routing around it.
- NEXT: resume R7 root cause (LSDA verdict pending; mid-function entry
  / prologue emulation / caller hunt).

## 91. RETSUCC-as-ship in APK (2026-10-13)
- PIVOT (decisive reasoning): unwinder catch is necessary for FAIL but
  the SUCCESS path throws nothing. Dead 2026 servers fail every real
  POST anyway -> canned sessions-200 (user-approved ship strategy)
  avoids the spiral entirely. Success continuation (functors/channel)
  with REAL Java callbacks is UNTESTED territory (harness starves it;
  device may supply it) — cheaper to test than fixing R7.
- BUILT: apphost setenv RETSUCC=1 + PROBE fill/raiser in OnLoad.
- NEXT: repack/reinstall/launch -> update#1 clean (r0=1)? sessions
  complete (flag?) -> Java callbacks arrive? -> burst/menu?? Binary
  per rung. If success stalls like harness (no burst) -> back to R7
  (fail path) with no regrets: both doors tried.

## 92. Main-thread IOException, no workers (2026-10-13, device APK)
- THROW (PROVEN): update#1 throws io::IOException (lr=0x2C86DC,
  FileInputStream region) EXACTLY once -> spiral (r0=0x45A778) ->
  0x39B1E8 cascade. Zero worker spawns (no thread-create lines) —
  game dies before networking. Real IOException = missing/unreadable
  FILE (which path? file-open lines just before the throw in the full
  log hold it — NOT yet pulled).
- NEXT (after maintenance): pull open/fopen context around the throw
  ([files]/[regio]/asset lines) -> name the missing path -> map it
  (files alias? asset STORED? CWD-relative?) -> relaunch. Then workers
  should spawn -> sessions -> (unwinder for fail | continuation).

## 89. LSDA verdict: frame confusion; live-frame answer test (2026-10-13)
- SUBAGENT (PROVEN bytes): entry 0x5BA760 = compact Su16 short (pr0),
  descriptors 97/42/AC + Finish, LSDA ABSENT (no pads/actions).
  Personality MUST return CONTINUE (never HANDLER). Advance SHOULD pop
  LR=0x457FE7 from [R7_45A758+8] -> __cxa_throw -> game frames.
- FRAME CONFUSION (PROVEN live values): SP=R7-0x20 + [R7+4]=0x45AAFB
  constant proves live R7 is GETINFO's frame, not 0x45A758's. pc
  (0x45A778, stale getcontext save) and R7 (current getInfo frame)
  come from different frames -> descriptors pop wrong words -> LR
  garbage -> re-present. My R7POKE/[R7-12]/0x457FE7 recipe was aimed
  at the wrong frame (all misses explained).
- FIX (built): answer for the LIVE frame (lr&~1): descriptors match
  live R7 -> coherent unwind through real frames. No guest touched.
  Binary next: new pcs/handlers/terminate vs same loop.

## 87. Pointer-width airtight + Call delegation (2026-10-13)
- BUG CLASS (PROVEN maps): ART lives high (0x7x-dominant) — raw 64-bit
  pointers must NEVER enter 32-bit guest regs/fields. Design: guest
  keeps OUR 32-bit table/handles; thunks delegate per-call (real reads,
  guest-side copies/cookies/wraps).
- BUILT: WrapObject/UnwrapObject (0xF1100000+ arena, stable per real
  ptr, null-safe) + entry-wrapping (thiz + L/[ params) + RealCallObject
  (resolve mock class/method -> real global/methodID (cached) ->
  AAPCS jvalue marshal (V/A/direct, J-align, unwrap-or-materialize) ->
  real A-form by ret (V/primitives/String-guest-copy/wrap) +
  exception-clear+log) + strings delegate (168/169) + mock JNIEnv* in
  forwarders + RealEnv hook. Tuned mocks stay as fallback (harness
  identical: hook null). RegisterNatives stays fake+log (stubs next
  phase; real-without-stubs = wild jump, never).
- NEXT: repack/reinstall/launch -> config BAD gone? updates proceed?
  sessions attempt (real net) -> throw? catch? burst?

## 82. Context compare: coherent march, same pc (2026-10-13, ctxcmp1)
- 1026 queries, ALL pc=0x45A778: R7 descends 0x48/iter, SP=R7-0x20
  always, LR=0x45AF67 constant. Coherent pairs marching together —
  NOT random garbage: driver re-presents the same frame in a fresh
  (-72B) context per iteration. Pop never transfers (R7 would jump to
  heap values). Poke verdict stands (78k identical, no advance).
- DIVERGENCE excluded: success runs also query (109x, same pc+answer)
  and move on — same query+answer, different outcome => divergence is
  DOWNSTREAM of FindExidx (personality/context/resume), bridge OUT.
- NEXT (static, subagent queued): decode entry 0x5BA760 fully +
  LSDA + what personality MUST return for this frame + what SHOULD
  advance it. Outcomes: (A) must-be-HANDLER + misread word -> one-poke
  fix; (B) must-CONTINUE + broken step slot -> forge or accept;
  (C) correctly-handlerless -> close catch-branch as impossible,
  pivot to success-branch/device-APK. No harness runs meanwhile.

## 83. Adjudication: WATCHPC fires, probes starve; mid-function entry
   (2026-10-13, adjudicate1/2 + midfn1/2 + callsite1)
- WATCHPC @0x45AF62 FIRES (4x, r0=pc, frames) while ALL slot-probes
  at 0x45AF30/34/38/62/74 + step sites show 0 exec. Two candidate
  causes: (a) probe BUDGET starvation (PROBE_N shared; callsite1 hit
  3001/3000 cap -> suppressed ✓ proven for that run); (b) genuine
  mid-function entry. midfn2 (budget 200k, clean full run): entries
  STILL 0, WATCHPC fires -> (b) STANDS with budget confound removed.
  RULE (load-bearing): end-of-run probe verdicts REQUIRE total <<
  cap, else void; WATCHPC (own cap-4) as cross-sensor.
- No static edge of ANY kind enters [0x45AF30,0x45AF70] (ARM BL/Bcond/
  BLX-imm, Thumb BL/cond/CBZ all zero) yet the block runs 1k-78k/run.
  => dynamic entry (blx-reg/functor) past the prologue (push/add/sub
  skipped) => R7 never set => vsp garbage => re-present loop. Stack
  frames are mixed-live (throw pads, dispatcher, account) — ordinary
  worker flight, not garbage.
- DEVICE DECIDES (next, cheapest decisive): hook getInfo ENTRY +
  0x45AF62 on fresh boot. Entry executes there => our caller path
  diverges (capture its bt -> replicate). Entry silent there too =>
  mid-entry BY DESIGN (prologue skipped everywhere) => fix = emulate
  prologue (IR SetRegister r7/sp + host mem writes) or set R7 at
  block, then watch walk advance.

## 84. Only 2 throws (system_error); pivot to APK config (2026-10-13)
- THROWCOUNT (PROVEN): RETFAIL run throws EXACTLY 2x, both
  system_error (NOT Http!), lr=0x44275B (throw-region caller), each ->
  one 513-spiral (retval=0x45A778). No rethrow recursion (would show
  many throw lines). Spiral is per-throw deterministic, type-
  independent. Driver loop ignores answers/pokes; entries/steps never
  execute; vsp likely FIXED-stale (re-reads getcontext-saved pc slot
  forever) while driver allocates fresh context/iter (R7 march).
- PAUSE unwinder (diminishing returns: 6+ runs, no advance). The loop
  needs its caller (dynamic, unscannable) or prologue emulation (IR
  work) — parked with repro recipes intact (r7poke/skipframe/stepwatch/
  callsite/adjudicate logs + R7POKE/SKIPFRAME/WATCHPC infra committed).
- PIVOT (movement toward menu): APK config BAD (getActivity-mock vs
  real use) — the update UD2 cascade starts there. Phase-2 delegation
  (Call*/GetMethodID/refs/fields delegate-to-real; RegisterNatives
  real + dynamic stubs) per §85 design. Unwinder resumes if APK needs
  the fail path (likely) with LSDA verdict + fixed-vsp theory in hand.

## 77. Exidx bridge EXONERATED; loop is driver-side (2026-10-13)
- TABLE SORTED (PROVEN, exact C++ replication in python): 0 inversions
  across 18184 entries; binary search returns 0x5BA760 for pc=0x45A778
  (matches live bridge). My two earlier contrary claims (1001
  unsorted / linear-says-0x5BFF00) were MY python unsigned-wraparound
  bugs — journaled as method lesson: replicate bit-exact, verify twice.
- pc=0x45A778 = literal pool INSIDE fn 0x45A758 (+0x20; garbage in both
  ARM+Thumb decode = pool, section .text-confirmed). Driver queries it
  from query #1 (never visits the throw site) with a VALID entry
  answer 1026x, never advances, sp descends -> nest-halt 513.
- IMPORTS (PROVEN inventory): sole unwinder import =
  __gnu_Unwind_Find_exidx. Raise/personality/VRS/resume all in-module
  (__cxa_throw 0x457F91T, personality 0x458859T). Only host touchpoint
  = FindExidx answer (correct). MCSM compare: their host has NO
  _Unwind_* symbols either (not even UND) — fully in-module unwinding
  CAN work under Dynarmic; ours gets garbage frames, not missing impl.
- STUCK HYPOTHESES (subagent decoding now): R7=0 frame base? heap
  stacks without guard gaps (walk never hits unmapped -> infinite
  frames instead of terminate)? resume never transfers? Awaiting
  verdict for the flip recipe (bridge answer vs register vs stack word).

## 78. Raise-driver loop decoded; flip recipe (2026-10-13, read-only static)
- CALL SITES (PROVEN): exactly ONE Find_exidx caller — Thumb BLX at
  0x45AF62, in func 0x45AF30 (getInfo wrapper; entry==0/len==0 checks).
  Phase1 = 0x45A758, phase2 = 0x45A80C (string `unwind_phase2`).
  Personality codes 6/7/8/9 = HANDLER/INSTALL/CONTINUE/FAILURE.
- CORRECTION (subagent PROVEN, my pool claim REFUTED): pc 0x45A778 is
  CODE (Thumb ADD after BLX getcontext @0x45A774; bytes match Dynarmic
  thumb32.inc). Walk starts CORRECTLY at the raise machinery (LR saved
  as IP) and never LEAVES frame #1. Entry 0x5BA760 VALID, compact:
  SET_VSP r7, DEC -12, POP {r4-r8,lr}. SHOULD pop LR=0x457FE7 ->
  advance to 0x457FE6 (__cxa_throw 0x457F90) -> game 0x28AE70 handler.
- MISFIRE (INFERRED): saved-LR word [R7-12] STALE (0x45AAFB/pool/
  deadbeef instead of 0x457FE7) — pop restores R7, PC re-presents.
  R7-only REFUTED (halt R7=heap yet stuck). SP descends ~245B/iter
  (nesting). Heap-no-guard secondary (no-terminate), not the loop.
- FLIP RECIPE: write Thumb 0x457FE7 to live [R7-12] (=R7+12-24) ->
  pop advances. INFERRED (0.34, needs ONE live recon: read word,
  watch Find_exidx pc change). If personality then non-8 (no handler
  in harness) -> terminate: boundary = LSDA/typeinfo + login state.
- NEXT (tomorrow): live recon run (read [R7-12] at spiral halt,
  no new code) -> if stale-confirmed, one write-test (stack poke,
  harness-only) -> binary (advance? where?). Then downstream
  (personality/handler/pad) or terminate-accept.

## 80. A64 abort: OUR Void-args; fixed + proven on device (2026-10-13)
- ROOT (read-only static subagent, PROVEN code): 1-arg CallHostFunction
  fills trailing slots with Void (ir_emitter 2874-76); arm64
  PrepareForCall routes Void -> LoadCopyInto -> GetImmediateAsU64
  assert (reg_alloc.cpp:515). x64 HostCall SKIPS Voids
  (reg_alloc.cpp:459) — hence PC-immune. Injector: POKE164/DIAG164 at
  0x283B5C entry (same block as killing pcs — translation units are
  blocks, not pcs). Same latent bug in ALL 1-/2-arg emissions.
- FIX (port-side, no thirdparty fork): 10 callbacks widened to
  (u64,u64,u64) (PokeGateByte/ShadowState/ManagerFlag, WatchSP,
  DumpR9, ResumeTrap, EventDecide, LastThis, EventEntry, SuccInject
  2->3) + emissions padded with ir.Imm64(0). PC smoke identical
  (54/54, 948, 0 LOUD). All future CallHostFunction MUST use 3-arg
  form (rule; x64 silently forgives, A64 aborts).
- PROVEN ON DEVICE: deep run (full combo + DIAG164 + functor probes +
  notify watch, RETSUCC) clean end, heap top printed. Same shape as
  PC (notify fires, functors 0, Login failed 0). Device deep runs
  UNBLOCKED. (This run was success-config by mistake for the unwinder
  question — throw needs fail shape; next.)
- NEXT: device FAIL shape (DNSALLOW, NO RETSUCC, throw-type probe +
  functor probes + WATCHPC=5d2f0 loginFinished) -> binary: spiral
  with real bionic (R7 reproduces -> fix here) vs catch/functors
  (wall was harness artifact -> APK road open).

## 81. Device unwinder verdict: spiral reproduces (2026-10-13, unwind5)
- DEVICE FAIL (real bionic threads, DNSALLOW, no RETSUCC): 2x
  HttpRequestException (lr=0x28AE70, same raiser tail), tid 9+12 die
  in the SAME 513-deep spiral at pc=0x45A778 (retval=0x45A778).
  Functors 0, loginFinished 0, clean run end. VERDICT: wall
  REPRODUCES on device — not a harness artifact. R7/frame fix needed
  in shared code (helps both). §75(c) CLOSED.
- STEPWATCH CORRECTION (PC): step sites 0x45A7B8/0x45A7E6 never
  EXECUTE (0 [probe]#N) though queries flow — the mapped phase1-step
  is NOT the looping path (my "step stuck" framing wrong; translation
  of Thumb blocks unmeasured — translog was off, no inference there).
  Loop re-invokes per iteration (SP -245B/iter). NEXT: probe getInfo
  entry 0x45AF30 + phase1 entry 0x45A758 with EXIDXLOG -> entries ==
  queries (re-entry loop) vs << (inner loop elsewhere).

## 79. R7 recon from archive: stale CONFIRMED; LR slot is [R7+8]
   (2026-10-13, retfailbuild.log, no new run)
- 4 spiral halts (2 threads x2): R7=heap (12d27da8/1339eb90/...),
  SP=R7-0x20, LR=0x45AF67, pc=0x45A778 unanimous. Words: [-12]/[-4]=
  heap-frame-ish, [-8]/[+0]=heap, [+4]=0x45AAFB CONSTANT all 4
  (real saved ret into step/getcontext region — frame PARTIALLY live!),
  [+8]=heap garbage (1f2350c0/1f285470/...). Expected LR 0x457FE7
  (BL @0x457FE2 ret into __cxa_throw) NOWHERE. STALE CONFIRMED 4/4.
- SLOT CORRECTION (pop semantics over subagent): compact 0xAC pops
  {r4-r8,lr} from vsp=R7-12: r4=[R7-12] r5=[R7-8] r6=[R7-4] r7=[R7+0]
  r8=[R7+4] lr=[R7+8]. LR comes from [R7+8], NOT [R7-12] — recipe
  location fixed (bytes over decomp-reading).
- WRITE-TEST (built: HARNESS_R7POKE=1, first 3 pc==0x45A778 queries,
  heap-R7 + writable check, logs old->new): poke LR slot [R7+8] with
  0x457FE7. Binary: walk advances past frame #1 (next exidxlog pcs =
  game frames 0x457FE6...0x28AE70...?) vs stands. If advances ->
  root-hunt the clobberer + downstream; if stands -> loop elsewhere.

## 75. APK-scope correction + headless-device middle path (2026-10-13)
- CORRECTION (my "predictable LOADING" OVERSTATED — user challenged,
  correctly): split three ways. (a) CERTAIN harness-only (mocks, caps,
  pacing, DNS shapes, fills/RETSUCC/pokes) — gone on device. (b)
  MECHANISM-STARVED (posters exist as code: 0x291A5C has 5 callers,
  queue machinery real; only TRIGGERS missing: Java callbacks,
  timers, WebView, dynamic natives) — on device REAL Java may supply
  them for free → APK could go FURTHER than harness, UNPREDICTABLE
  without building. My LOADING-prediction assumed triggers stay dead;
  that assumption is harness-colored, not proven. (c) UNWINDER R7 —
  genuinely unknown on device (Dynarmic-context vs harness-thread
  artifact): the throw path runs the same guest code, but our threads
  (bump-heap stacks, emulated TLS) differ from bionic threads. Settle
  empirically, not by assertion.
- MIDDLE PATH (no full packaging): headless arm64 harness on device
  via scp (tunnel exists; Termux+su runs NDK PIEs). Toolchain ready
  (arm64-v8a/api-26, libdynarmic.a built Oct 9); harness target builds
  for Android too (only gltest/eglmin are PC-guarded; guest links
  EGL/GLESv2). Push harness + guest.so + files + APK, run with shim
  GLES (no X11) and watch throw->catch/functors with REAL bionic
  threads. Settles (c) for hours of work vs days of packaging.
- ORDER: (1) headless-device unwinder test; (2) if catch works ->
  full APK becomes rational (triggers free); (3) if spiral reproduces
  -> R7 workstream with device-grounded evidence. Harness stays the
  fast loop either way.

## 76. A64-backend abort forensics; unwinder back to PC (2026-10-13)
- ABORT (device-PROVEN, 3 runs): `GetImmediateAsU64 on incompatible
  Value` (asserts ON build) while translating manager 0x283B5C region
  (last [tr] 0x283B78-84; beat pc=0x283B5C lr=0x25142C). Same code runs
  clean on x86_64 (manager every render). Probes exonerated (abort
  without throw/functor probes; trans1 c1550,c19f8 clean).
- SOURCE-LEVEL (computer, free): assert callers = IR const-prop pass
  (backend-INDEPENDENT — would abort on x86 too; exonerated) +
  arm64/riscv64/x64 reg_alloc accessors. So it's the A64 reg_alloc
  assuming a constant operand — backend-specific bug (or our IR-hook
  interaction at those pcs — none armed there; DIAG164 only at entry).
  Suspect pattern: predicated ldr/ldrb+cmp+beq run (0x283B84 beq /
  0x283B88 ldr r0,[r10,#172] + ldreq/cmpeq after). NOT proven — needs
  a short-prefix repro (~90s/run) + workaround hunt (can't skip manager;
  no upstream fix: HEAD==origin/master 2026-09-26).
- USER HYPOTHESIS (x86-path vs arm64-path incompatible): PARTLY TRUE.
  Same IR/callbacks/guest (inits 948 SVCs identical!), but backends
  differ in maturity (x64 battle-tested, A64 young). Not "incompatible",
  but expect A64-only backend bugs like this one.
- DECISION (easiest path): unwinder root-cause BACK ON PC (relthrow
  2/2 reproducible in-harness; no device/backend involved). Park the
  A64 abort with repro recipe (full combo + TRANSLOG; abort ~svc1100
  at manager translate). Next: raise-driver 0x45A778 + FindExidx
  bridge interplay (why 500+ queries from one pc?) — bytes + our code.
- All 5 read-only (no runs/device/writes), jev-scored, byte-evidenced. My spot-check
  (python, LE bytes): BC048=eb000017, 5FEE0=ebfff502, 5EBE0=ebfff9c2,
  284078=e589a0a4, 466F34=344087e5, 46B618=0660a003 — ALL 6 MATCH.
- A1 (0xBC100): ADS 0.97. 0xBC100 = interior (+0x54) of Lua addPlacementNative
  binding (reg triple @0xBA478 byte-proven); chain addPlacementNative->0xBBF64->
  0xBBFD4->0xBC0AC->0xBC158. Backtrace edges REFUTED in bytes (0x63B54=tst,
  0x388904=add, 0x63EE4 never BLs loginFinished). §40 DEAD (§39 backtrace was
  stale-lr garbage). RULE: never call 0xBC100. True fail site: BL @0x5FEE0
  (functor 0x5FE70 -> loginFinished(ctx+4,0)); success site BL @0x5EBE0 (r1=1).
- A2 (sessions chain): NO covering typed pad 0.87. BARE dispatch verified
  (0x28B140 no fork; this+0x28 empty -> functor assembly skipped @0x28A680-6CC).
  Throw A/B -> PLT 0x53708 -> GOT 0x670E60 -> Thumb __cxa_throw (PLT math python-
  verified). extab/LSDA python-decoded: 0x28A3DC 36 sites act0, throw rets in
  pad-0 sites; neighbors cleanup-only; 0x28DAF4 compact no-pads. Functor reads:
  (ctx+4,0,ctx+8)->loginFinished, needs ctx+0xC string flag; NO cmp #0x11
  anywhere in 0x5FE70-40 => [+0x14]==0x11 needle SUSPECT (explains RAWPOST-no-
  menu: clean call, wrong object). Record claim REFUTED: FUN_0009fed4 = file
  0x8FED4 (analytics new(0x74)/new(0x28)), no new(0x2C) — another 0x10000 slip
  in MY prompt. True ctx provenance OPEN. "Never derived" globally FALSE (one
  derived catch in 0x19FE80) but TRUE for dispatcher chain.
- A3 (builders): trigger recipe 0.71. CORRECTIONS: FUN_000d45fc = file 0xC45FC
  (not 0xD45FC — my prompt slip, agent caught); DAT_ addrs are GHIDRA VAs too
  (rule now uniform: Ghidra-0x10000=file, code AND data). Builders zero direct
  BL (full ARM+Thumb32 scan) — virtual via 0x6684/0x400 tables (0x41F9B0 entry
  = Thumb-code false positive). Activation: nativeUpdate 0xC2014 -> BL 0xC2140
  -> walk 0xC45FC (event-vector non-empty + pi[3]==0 + +8 gates -> +0x18 fan-out)
  -> surface loop 0x250018 -> builders. r1=0x101929 PROVEN non-static (zero
  hits everywhere) = dynamic scene key. r1=1/r1=2 BL sites: SIX (extra 0x2616DC
  in loading fn 0x261468 was omitted).
- A4 (BL recipe): 2-reg recipe REFUTED (P~0.15, faults on ldm/memcpy/vector).
  Exact 7-arg recipe 0.72: r0=mgr(heap new(0xDC)) r1=1 r2=0 r3=zeroBuf>=72B
  stack 0,0,0, quiet point. Full decode: virtual getter ([mgr+0xA8]->vt+0x60),
  helper 0x283AC0 must return !=0 (empty => immediate 1), tail 0x28402C (empty
  re-check + string-swap no-ops when 0 + 156B memcpy + B4 grow). BSS-slot claim
  NUANCED: slot 0x672E98 holds mgr POINTER (live-proven across runs); agent's
  absolute-scan refutation is method-weak (slots read via pcrel literals). Keep
  slot reads + heap-scan cross-check. 0x283C48 fork = r1-vs-3 select, NOT the
  store gate (true gate 0x284058-64).
- A5 (resolver): park-not-throw 0.87. 185-hit cluster = curl poll loop over 4
  easy handles (0x466F34 = memcpy-return str), NOT threads — old inference
  killed. 0x468C9C = park (r5=0); 0x468D70 = fail (r6=1, iVar12=6). Code 6 built
  @0x46B618 (moveq r6,#6) only on COMPLETED failure (retries 4xnservers
  exhausted via FUN_004A6890); pure silence = in-flight = park. Timer gives 28,
  never 6. Retries TRUE (bounds verified). Consequence: failure path needs
  minute-scale exhaustion; DNSHUSH-no-throw explained.
- VA RULE (final): GhidraVA - 0x10000 = fileVA, uniformly. My prompts carried
  two 0x10000-class slips (0xD45FC, DAT_ rule) — agents caught both; verify-all
  stands.
- REVISED PLAN (serial): (i) ctx provenance (static ctors of 0x5FE70-ctx class
  + callers of 0x5FEE0; live WPA on ctx+0xC writes); (ii) event-vector filler +
  element format + live post (gate at 0xC45FC entry); (iii) 7-arg BL as scene-
  liveness diag (expect WHITE — proves liveness, not menu); (iv) code-6 long run
  only if failure path revived. KILLED: 0xBC100 calls, 2-reg BL, timer-28,
  [+0x14] needle criterion, thread-attribution of poll loop. NON-NEGOTIABLE #1
  SOFTENED (user-approved): fabrication allowed as scaffold AND ship candidate
  (canned offline session for dead 2026 servers); honesty via logging, ship
  criterion = offline menu+play, not method purity. #3/#4/#5 stay strict.

## 45. Walk interrogation: empty vector proven live (2026-10-12, walk2.log)
- Walk 0xC45FC runs EVERY update (27x, [probe]#N execution dumps, NOT
  probe-emit). Guard fails EVERY time: r0=0 at 0xC4710, no fault => impl valid,
  subscriber count *(impl+0x10)==0. Invoke 0xC4830 NEVER runs (zero dumps with
  fitting lr). Vector EMPTY the whole run. r1=0x672DA8 const, entry r0 =
  EventProcessor (vtable 0x66B7B8, class sniff). impl = *0x672DA8 = heap
  (0x10000A0C here — drifts, runtime discovery mandatory, never hardcode).
  Sessions succ + flag=1 as usual; +164=2; film RIO LOADING (real art).
- INFRA LESSONS (burned, load-bearing): [probe-emit] = TRANSLATION time,
  [probe]#N = EXECUTION (census1/2 counts were translation noise; walk2 verdict
  rests on 56/56 accounted execution dumps: 27 entries + 27 fails + fill +
  raiser). PCTRACE = translation-only, NEVER execution evidence. ProbeDump cap
  (PROBE_N) is shared across all 5 slots. Slot budget: fill + raiser
  UNTOUCHABLE (dropping fill = ParseError death at update#1, main-thread spiral
  with R7 in MAIN stack, same stale shape => spiral class is thread-
  independent, context-init — census1/walk1 died this way TWICE). 3 free slots
  max. zsh: no `echo ===`, grep brackets need -F.
- Subscribe executions: census2's "2 hits" were translations; walk2 shows ZERO
  subscribe execution dumps (init#50 + void-event 0x18D734 paths vary run to
  run — OPEN, not load-bearing). Neither subscriber is EVENT_UPDATE.
- NEXT: HARNESS_EVENTPOST (new, minimal): at walk entry, if count==0, build
  synthetic pi/arr/elem/callable in guest heap from LIVE slot reads
  (*0x672DA8, never hardcoded); callable +0x18 -> new safe SVC slot (logs
  marker + Ret, walk continues, update completes). Binary: marker in log <=>
  walk invokes our event. Design only — implement + build + run next.

## 46. Synthetic event INVOKED 27/27 — delivery proven (2026-10-12, eventpost4)
- INVOKE x27 (every update): walk -> synthetic elem -> 0xC4830 -> forged
  vt+0x18 (free thunk) -> marker (a0==callable) -> Ret -> walk continues ->
  update completes clean. Zero crashes. Baked-r6 design SOUND (entry builds
  before stash-block translates; refresh same-addrs; mirror-first so natural
  content is never clobbered). args=(callable, stack, stack, f0000020).
- CORRECTED MODELS (all byte-verified): (1) hook reads r6 BEFORE mov — stash
  must use r0 (natural pi), never r6 (stale update leftover 0x672E6C);
  eventpost2's "NATURAL EVENTS" was BSS garbage via stale r6, not game state.
  (2) pop-into-pc epilogues leave lr STALE — lr is NEVER evidence without a
  call-edge audit (killed my catch-resume theory AND explains §40's ghost
  backtrace class). (3) PreCodeTranslationHook is PER-INSTRUCTION pre-insn:
  any reachable pc is hookable ("mid-block never fires" lesson REVISED: it
  never fired because never reached). (4) 0xC474C returns NULL normally
  (empty map, mov r0,#0 path) — no throw on this path; EE4swap lines were
  ProbeDump r1-sniff artifacts (no such typeinfo in binary).
- Subagent's "harness-synthesized resume" REFUTED (nothing in harness writes
  pc; the stale-lr normal return explains all); its byte facts (no pads, NULL
  path, corrections 0xC4910/0xC4BA8/0xC4D24, mirror recommendation) ACCEPTED.
- sesswatch=132 this run is ARTIFACT (DUMPR9 armed on OUR synthetic elem via
  c4830 slot) — flag meaningless when DUMPR9 fires on synthetic invokes.
- REMAINING GAP (crisp): delivery works, CONTENT missing. Real subscriber
  functors unknown (21 subscribe sites; scene's unidentified). NEXT (parallel):
  (i) static audit: per-site event arg + functor vtable + enabling condition
  -> the scene/EVENT_UPDATE subscriber; (ii) live 7-arg BL 0x283A3C via
  HARNESS_SCENEAT (mgr from PokeManagerFlag's last_mgr_, validated + empty-
  vector-gated) as scene-liveness proof (expect WHITE: flip without builders).

## 47. SCENE7: flip proven live, white as predicted (2026-10-12, scene7.log)
- 7-arg BL @call25: mgr=10002f98 (hook-observed, never hardcoded), +164==2,
  B4 empty [12b2dcb0..12b2dcb0] => hr=UD1 clean, r0=0, +164: 2->1, HELD to end
  (single change-line; loading did NOT re-assert). Film: logos -> WHITE after
  the call (user-confirmed). Zero BAD/FATAL/LOUD. Zero builder translations
  (no 0x250xxx emits post-flip). Transition pinned: call24 = FOX art, call25
  = flip, call32+ = white (flip landed in FOX phase, same white verdict).
- Verdict: scene-liveness PROVEN (flip takes effect visually), content absent
  (builders never activate). Backstop closed as predicted — NOT menu. Menu
  needs builders = real subscriber functors via events.
- INVOKE=0 here is EXPLAINED, not a regression: PROBE4/5 unset this run, so
  EVENTPOST hooks never wired (slot budget). Slot rule stands.
- NEXT: subscribe-site audit (background subagent) -> real scene subscriber
  -> enabling condition or direct subscribe-path call -> real-subscriber event
  post through the PROVEN delivery (mirror path already handles natural
  content when present).

## 49. Login initiation found + VA-data correction (2026-10-12)- Static (read-only subagent, byte-verified): loginRovioId = Lua-only
  registration (native_RovioAccount table in ctor 0x5CB28 via FUN_0028EBFC);
  zero BL to functors (vtable/event dispatch only); enabling condition
  +0x19==0 (FUN_0006CE50 gate; loginFinished clears); minimal trigger = BL
  0x5CE50(r0=account). loginWithUI separate, no edge. 0xBC100/ads confirmed
  again (backtrace refuted twice).
- VA-DATA CORRECTION (load-bearing): data file = VA-0x1000 (PH3), code
  file==VA. So: scene-vt VA 0x669428 (values incl. builders per table),
  GL_Image vt VA 0x669620, account vt VA 0x65E504 (needle was VA-correct).
  My file-offset reads of data were off by 0x1000 (e.g. [+100]=8 garbage was
  VA 0x66A48C, not the slot). RE-READ: [scene-vt+100] = file 0x66848C =
  0x251308 (valid code!) => forged scene-this would proceed past the first
  virtual. r1=0x101929 is CODE (not data) => stale/int-token, safely passable.
- Trigger decoded (self, distv): 0x5CE50 guard +25==0 -> set 1, reads +32,
  big call +#518028, virtuals. Forged zeros satisfy the head. NEXT: live
  HARNESS_LOGINAT (forge account + BL 0x5CE50) -> login flow start?
- Builder this: loading-this = GL_Image (wrong class for menu builder);
  menu this must be scene-class (0x669428 family) — absent in heap (never
  constructed). Direct-builder shelved behind initiation (needs graph).

## 50. Heap starvation + initiation ladder (2026-10-12)- HEAP 99% FULL (PROVEN live): top 0x1F226820/256MB at exit, deterministic
  across runs (0x1F226820 vs 0x1F2266D8 = 72B). Leak-only free + textures hit
  the ceiling ~call 30; mallocs return NULL -> null+off faults (loginat clone
  dest 0x50). FIX: kHeapSize 256MB->1GB (arena ends 0x50000000 < mmap
  0x70000000, safe) + [heap] top print at exit. Post-bump: same top (usage
  fixed ~254MB), fault GONE, trigger runs clean to hr=UD1, full 63 calls.
  REFRAME: every post-~30 verdict is suspect under 256MB (null-malloc
  artifacts: marathon deaths, worker flakes, BAD reads).
- Initiation ladder (live): LOGINAT=31 clean no-op (no Login/builders/film).
  loginat3 caller probe: clone dst r1=0x50 (null+0x50), lr=0xDBF80.
  loginat4/5: heap verdict + clean run post-bump, still no Login/builders.
  NEXT: LOGINAT=11 (workers alive) + functor probes (5f96c/5fe70) — timing
  hypothesis (device initiates at boot-seconds, not mid-park).

## 51. Unwinder CLOSED, ladder continues, two statics queued (2026-10-12)- R7=0 AT THROW ENTRY (PROVEN live, r7bisect1: 2 throws, R7=0, R11/SP valid
  stacks, lr=28AE70 raiser tail). + Find_exidx verified + raise in-module +
  pop-into-pc epilogues (lr always stale) => unwinder path UNAVAILABLE, not
  just broken: VRS[R7]=0 poisons step 1, true R7 unrecoverable post-hoc, R7
  games would only change the death shape (END_OF_STACK vs spiral), never
  reach pads. Offline (catch-needing) path CLOSED. Success path (zero throws)
  is the only viable one. Rule: no more unwinder work.
- Loginat ladder (live): +32-block -> same fault (base NOT +32: exact addr
  0x50); caller probe (0x5F430: r1=0x50, lr=DBF80); heap EXONERATED (same top
  post-bump, fault persists => null-object, not malloc); WATCHPC built (regs
  + backtrace, no slot); chain = trigger -> big -> update-code (C524C) ->
  D59CC/D5D40 -> tree-op -> clone; D5994 gets NULL in trigger, live object
  0x10071FA0/0x10072220 in updates; +32 linked live => trigger runs CLEAN
  (no BAD, full 63 calls) but sterile (no Login/builders/film change).
  Forged zeros flow down as NULLs; real session linkage fixes the fault but
  not the outcome. Initiation works mechanically; functors never materialize
  (5f96c/5fe70 silent even early).
- Main observes (static): gates #1/#2/#3 ONLY — no network/session/queue.
  Trigger is calls, never data. Queued statics: (S1) time/apdrive consumers
  + notify + direct-call recipes; (S2) gate#2 (+148) object + writer +
  downstream + recipe. Next live depends on both (no decisive angle pending).
- Infra: WATCHPC (+WatchSP, wpc_shared), loginat_session_ stash+link,
  [heap] print, [env] fingerprint — all committed, harness-only.

## 48. Subscribe audit + functor verdict: channel, not call (2026-10-12)
- Static (read-only subagent): helper 0x3833B8 binds NO event — event comes
  from the functor in both shapes (wrapper-fresh and direct-existing). All 21
  sites bind void(void)-family; UPDATE (Event<void(float,Context*)>) has only
  2 refs in binary (create _INIT_3 + publish 0xC2140). NO static UPDATE
  subscriber (0.96). Live: 13 subscribe executions/boot (init, void x7, ads
  x2, wrappers x2 with r2=processor not event), none UPDATE. +164 writer is an
  EFFECT of builders (r1=1 sites inside builders), not a trigger — causality
  flipped: build menu to flip +164, not vice versa.
- Live functor (functor1.log, forged __func ctx via RAWBUILD): 0x5FE70 runs
  CLEAN (hr ok, "Login failed" x1 logged, post [+0x18]=0), film RIO LOADING,
  +164=2, zero builders. Forged ctx suffices (no real account needed for clean
  execution+log). Verdict: MISS — the functor CALL is sterile (like direct
  loginFinished: r2=0 skips +40/+16 reads, one +19 write + log).
- REFRAMED GAP: trigger is the completion CHANNEL around the call (job tail
  notify/post/wake/loop-continuation), never the call. Sessions SUCCESS also
  stalls (flag=1, registry saved) => channel dead in both outcomes =>
  independent of login/catch/unwinder. NEXT: T2 static (process [+12] branch
  + writers + functor epilogue + job tail + consumers + minimal recipe).

## 43. Unwinder forensics + resolve-branch + deaths (2026-10-12, pre-compression)
- SPIRAL MECHANISM (live): death = 500+ Find_exidx(pc=0x45A778) from FIRST
  query (no early walk; 1026 unanimous). 0x45A778 inside raise driver
  post-context-save. Context: phase-2 loop (FUN_0046AF30 calls Find_exidx per
  step). R7 at halt = heap (worker stacks live in heap arena) with heap word
  in lr slot (stale frame). EXONERATED (verified): exidx table+search (bridge
  == independent linear scan); heap/stack overlap (bump discipline, NULL on
  full, no wrap); Ret()/T-bit (Thumb BL sets lr-bit, mechanism correct);
  SVC-trampoline poisoning (no guest frames carry tramp pcs, mechanism).
  OPEN: why VRS/R7 holds stale (Dynarmic context-init? clobber class?);
  personality behavior under emulation. Builds: EXIDXLOG + R7-dump diag
  (committed, keep for one more round then drop).
- RESOLVE BRANCH (work/resolvebranch1.log, nodns): fail site 0x468D70 AND
  success site 0x468C9C BOTH translate; 185 executions cluster (lr=0x466F34,
  r0 = 4 heap states ~even). INFERRED (weak): success x3 threads vs fail x2
  threads (retranslation counts). Per-hit site attribution OPEN (no pc tags).
- DEATHS NEW: marathon3 (long, early taps): time worker tid3 DIED (BAD read
  @0x50006358, pc 0x470D90 resolver region, r4=wild) — first time-death ever
  (was always park). relthrow shape: tid9+tid12 die via unwind 2/2 reliable.
- MISSES (recorded, no menu): devcache (device 288B registry, P2 dead),
  devid (device ANDROID_ID, still LOADING), nopoke (poke theory dead),
  rawpost+clear (flag cleared, no menu), REDIR-functor (ran, BAD crash).
- Builds in tree (all committed): DNSHUSH, HEXFINDHEX, RAWPOST/RAW1/snapshots,
  ParkRedirectObj/REDIROBJ, EXIDXLOG, R7-dump. Binary == tree HEAD.
## 42. REDIR->functor executed, then BAD crash, no menu (2026-10-12)
- Build: ParkRedirectObj (r0 override) + HARNESS_REDIROBJ=<needle> (heap scan
  + [+0x14]==0x11 verify, else plain redirect). Config: reliable-throw
  (servers+sendfail, no RETSUCC) + PROBE2=457f90 + REDIR=5fe70,5fe70/REDIRN=2.
- LIVE (work/redirfun1.log): BOTH throws redirected (r0=record 0x10AD4F10
  found by needle); fail-functor ran; loginFinished logged
  ("Login failed: 279793516 => ..."); then [mem] BAD write @4 at pc 0x457FAE
  (inside __cxa_throw region) on both threads; threads halted (done with heap
  retval, NOT spiral-death). Film: LOADING. +164: 2.
- VERDICT: step 1 MISS (no menu) + crash side-effect to root-cause (null-r0?
  operator new/delete stubs? return-to-raiser-tail?). NEXT per plan: step 2
  (direct +164=1 via BL 0x283A3C) — awaiting go.
## 41. Ten-subagent synthesis (2026-10-12, all read-only, all completed)
- Branches DEAD (proven): Lua initiation (never calls; only path, silent),
  time/apdrive relevance (park forever, zero device lines, no consumers),
  update gate (both states stall identically; +1187 = quit-subblock only),
  missing mocks (no halts ever), 0xBC100-login (ads binding, correlation).
- Branches LIVE:
  1. +164 writers: EXACT store (str r10,[r9,#0xA4] @0x284078 via 0x283A3C,
     emptiness-gated); r1=1 sites virtual-only (scene walk); recipe = single
     BL 0x283A3C(r0=mgr,r1=1) when vector empty. [REFUTED 2026-10-12, see
     §44/A4: needs 7 args (r2=0,r3=zerobuf,stack 0,0,0); mgr is heap, not BSS;
     2-reg form faults. Proven live by SCENE7, see §47.]
  2. loginFinished entries: ONLY the two functors (0x5EBA4/1, 0x5FE70/0);
     virtual via ce50/eb658; no JNI/Lua-direct path. Cheapest offline =
     fail-functor with a record.
  3. Functor/record construction: record = new(0x2C)+init @FUN_0009fed4
     (gate outer+0x34C==0, boot init — runs in ours, record EXISTS with
     +0x19=1); stack functors via ce50 (gate +0x19==0 first! then set 1);
     heap clones inside eb658; async outcome needed after.
  4. Sessions dispatch is BARE by construction (tag 2, no fork; config
     this+0x28 empty -> functor assembly SKIPPED in raiser). Sync throw flies
     out; single covering pad swallows. loginWithUI is the ONLY functor-
     creating path (needs UI).
  5. Main observers: main reads NO network channel statically; menu = scene-
     activation mechanics (+164 builders r1=1 vs r1=2, event vector
     FUN_000d45fc), not data delivery.
  6. Unwinder spiral: 0x45A778 INSIDE the raise driver (post-context-save);
     exidx entry CORRECT (SU16 vsp=R7); top hypothesis = bad initial R7 from
     context-init under Dynarmic. SVC frames provably innocent.
- CONVERGED NEXT (uses only proven pieces): REDIR the reliable sessions-POST
  throw -> fail-functor 0x5FE70 with OUR runtime-found record (RAWPOST proved
  the call runs clean; REDIR lands THROUGH pads whose side effects the direct
  call skips). Binary: menu/progress (chain lives) vs same stall (dead).
- EXECUTION ORDER (agreed, strictly serial, verdict after each):
  1. REDIR throw -> 0x5FE70(record). 2. If miss: direct +164=1 via BL 0x283A3C
     (scene-liveness proof). 3. In parallel (static): R7-spiral root cause.
  4. Do NOT touch: time/apdrive, +1187 gate, mocks (except 2 cosmetic VALUE
     fixes opportunistically), Lua initiation, 0xBC100.
- MOCKS VERDICT (2026-10-12): leave as-is. No evidence any mock blocks;
  networkType tested live; Locale/Build divergences cosmetic until proven
  otherwise by a failing branch (none observed).
## 40. Device per-update login poller 0xBC100 (cold boot 5445, full capture)
[REFUTED 2026-10-12, see §44/A1: 0xBC100 is the ads binding, not a login pump;
the capture's chain was stale-lr correlation. Do not call 0xBC100.]
- 0xBC100 runs INSIDE Java nativeUpdate (GL thread, jstack: nativeUpdate <-
  doUpdate <- onFrame <- onDrawFrame <- GLThread), repeatedly, constant args:
  r0=heap (stable per boot, == ctx+0x10!), r1=module-static (file 0x69F2C),
  r2=0, r3=0x20(32). Downstream (same tree): 0x388904 -> 0x63B54 -> 0x63EE4
  -> loginFinished(ctx,0). It IS the per-update login pump.
- ctxdump (ctx+0): vtable-ish file 0x612504; +0x10 = bc100-r0; +0x18..:
  `01 01 53 57` => +0x19=1 (in-progress, pre-clear, both attempts).
- Gate settles to 1 after loginFinished pair; menu follows.
- OUR HARNESS (work/ourbc100.log, full body + poke): 0xBC100 NEVER translates.
  Its call is conditional on state ours lacks. Zero static callers (ARM BL /
  BLX-imm / Thumb BL / absolute refs) -> reached intra-function or indirectly.
- NEXT: subagent decoding fn 0xBC0AC (args/callers/subtree/success branch);
  then replicate the enabling condition or call it directly.
## 38. Frida on device (2026-10-11 eve)
- Host: project-local .venv (python3.13), frida 17.23.3 first (wheels ok),
  then 16.7.19 + frida-tools 12.3.0 (frida-tools CLI broken on 3.13 —
  top-level `import _frida` fails; use lib API directly, it works).
  Server origin: github.com/frida releases over TLS (official), ELF sanity
  checked (Android arm64 PIE, NDK-built). NO system-wide installs.
- Device (SSH u0_a41@192.168.1.202:8022, Termux, root via sudo): frida-server
  runs as root, listens 127.0.0.1:27042; host reaches it via ssh -L tunnel
  (KEEP the tunnel process alive for all Frida work). Game NOT running at
  setup; spawn broken in vphoneos (helper-exec fails, activity started but
  process died on start-timeout — recovered via plain am start, pid 1801).
- INJECT MATRIX (live): Termux bash (64-bit, own uid) OK; Gboard (64-bit app)
  OK; game (32-bit, vphoneos emulation) FAILS identically on 17.23.3 and
  16.7.19 (helper-exec error). Container API 25. Conclusion: 32-bit inject
  broken and/or game-context-specific. NEXT: user creates pure 32-bit sandbox
  to split the two; fallback = gadget-repack route.
- GADGET ROUTE (live, works): repacked COPY (apktool 3.0.3 + smali loadLibrary
  in App.onCreate + libfridagadget.so arm32 + listen-config :27044, debug-
  signed via jarsigner; engine lib md5-identical 9c21a987). Installed in
  sandbox (uninstall wiped saves, approved). Gadget LISTENS on 27044 via
  tunnel — no ptrace needed. Hook scripts attach fine.
- vphoneos HIDES the engine: no process maps libAngryBirdsRio.so (file bytes
  loaded privately by the emulator). Hook-by-module impossible; going by
  byte-pattern memscan (nativeConfig@0xC1550 + sessions@0xD6C78 16B needles)
  to find engine base, then hook by raw address.
- PARKED (do not run yet): device JNI-boundary capture (all Call*/Get* with
  names+returns over boot->menu via Frida) -> diff vs our mocks -> fix mocks
  1:1. Subagent CAN do the static half anytime (expected-truth table per
  method from AOSP/JNI docs + pre-diff our mocks vs docs for definite bugs);
  device capture + diffing stays manual (live timing).

## 93. Device night run: seeds→global-fix→shader-gate→RENDER_READY→/session/ frontier (2026-10-13)
- SEEDS (PROVEN): device files dir had ONLY guest.so+log. Missing
  highscores.lua threw io::IOException on main thread (update#1, no
  workers). Pushed harness seeds (highscores/settings/bi_data/cacert +
  empty registry, u0_a46:u0_a46) -> throw GONE, boot proceeds.
- STALE-LOCAL (PROVEN tombstone): Context.getAssets() on cached Activity
  -> ART abort "stale Local 0x100005" (CallObjectMethodA via
  RealCallObject@dispatch.cpp:5320, addr2line-verified). FIX (f297bae):
  WrapObject promotes to NewGlobalRef on Android + IsSameObject
  identity scan. PC untouched.
- SHADER-DUMP ABORT (PROVEN tombstone): game reached menu-shader compile
  (2d-sprite-alpha-masked, pp.fx) -> harness-only work/shaders stash ->
  create_directories("work") on CWD=/ -> filesystem_error SIGABRT.
  FIX (345d18c): #ifndef __ANDROID__ gate. Audit: only throwing
  create_directories (others take ec); screenshots live in PC-only realgl.
- RENDER GATE (PROVEN Frida, FULL server, no gadget): state b=
  RENDER_READY; SingleThreadWrapper mIsLandscape=true,
  readyToRender()=true; onDrawFrame 299x/15s (GL thread alive). Black
  screen = our faked gl* (nothing reaches the driver), NOT Java gates.
  (MultiThreadWrapper has no instance; frida-server-16 matches .venv
  16.7.19, -17 mismatches; process name "Angry Birds"; tunnel -p 8022.)
- FRONTIER (PROVEN log): sessions-succ (plural) completes, tokens saved
  to device registry, game builds SINGULAR /session/1/apps/... request
  (authenticated, stored offline-test token) -> unmatched by RETSUCC
  gates -> real TLS (CF 13.32.x.x:443, CHello 0301/0301) -> stall.
  FIX (30e8d67): /session/ branch with sessions schema; iterate via
  KeyNotFound. SECONDARY GAP (noted, untouched): apdrive reads
  /tmp/opencode/fakeserv/variantconfig.bin (absent on device).
- LESSONS: "ended" logs only on hr!=UD1 (clean = silent); call# logged
  only for first 40 (renders invisible after); "recommended" contains
  "ended" (grep carefully); &&-chain committed broken code once (grep
  -c error matches -> chain continued; amend fixed, tree green).
- NEXT: /session/ verdict (completion? KeyNotFound key? loginFinished?
  burst?) -> then apdrive path + real-GLES passthrough (share-group
  design) for pixels.

## 94. Update driver stops after 34 updates in 1s; zombie verdict (2026-10-13)
- PROVEN (logcat timestamps + file count): 34 nativeUpdates ALL inside
  23:00:25.9-26.x (first boot second), then ZERO for 7+ min. Count 34 <
  40 (call# cap) -> genuine stop, not log truncation.
- PROVEN (task list, 21 tasks): no UpdateThread, no guest workers (all
  done retval=0 / parked in poll). Main 8305 alive+sleeping; 7
  GraphicsThreads + 2 RenderThreads alive; 8333 caught Running (black
  frames, likely unthrottled swaps with no content).
- PROVEN (Frida live): state b=RENDER_READY (not EXITING); no tombstone;
  no logcat exception; screen Awake; App resumed.
- PROVEN (smali): doUpdate-nonzero does NOT exit (access$200 -> loop).
  Silent Java death would log; none logged -> "34-in-1s then stop"
  reads as FINITE init burst, not loop death (INFERRED, needs the
  birth-hook run to settle).
- Last main-thread guest acts: settings/bi_data saves + Flurry onEvent
  (real delegation OK). Workers done; GL spins black.
- CORRECTION (2026-10-13, process 8305 GONE with no tombstone): the
  "34 then zero" reading was a LOG-CAP ARTIFACT (same bug class as
  §83 probe budgets!). call# logs only while calln<40; 6 non-updates +
  34 updates = 40 EXACTLY -> updates #35+ ran UNLOGGED. Workers kept
  going ([succ] sessions 4x + time 2x + [poll-ret] prove later
  activity); utime burned; THEN the process died silently (no
  tombstone, no exception). Zombie verdict above: RETRACTED as
  "frozen at 34" (updates continued unseen); the DEATH itself is real
  and unexplained (clean exit? watchdog _exit? Java quit path?).
  doUpdate-nonzero loops (access$200), does not exit the thread.
- LESSON (load-bearing): call#-style caps hide continuation; any
  "stopped" verdict needs an uncapped sensor (raise caps on Android
  first, conclude second).
- NEXT MEASUREMENT: relaunch + Frida-from-birth (UpdateThread.run/a/
  doUpdate entry-exit-returns + starts) -> name the stopper. THEN
  fastmem (user-approved after analysis).
- RESULT (Frida birth-hooks, pid 9480): doUpdate TRUE fires REPEATEDLY
  from boot (guest r0=1 every update -> Java sets EXITING, loops via
  access$200, never dies); UT.run enter NEVER fired -> updates come
  from a finite init burst (main?), UpdateThread never starts. Gate#3
  (+1187) never opens on device -> idle-bail forever. RENDER_READY
  (doInit overwrite) masks EXITING. THE device wall = +1187 writer
  0xBE704 never invoked (PC: POKE1187 forced it; device needs the
  natural caller).
- SUBAGENT (read-only static, PROVEN bytes): 0xBE704 = OSInterface
  gate-setter ([+1187]=1 always, [+1188]=r1); sole static path =
  wrapper 0x74168 -B-> 0xBE704 (r1=0); 0x74168 has ZERO static edges
  of any kind (BL/B/Thumb/absolute/init/got/pools all 0) -> dynamic/
  computed callback. No vtable holds 0xBE704 (both OSInterface tables
  skip it). Poller 0xC2144-50 (BL 0xBE714, BNE bail) confirmed. R1=1
  path: no static source (word 1188 stays 0 naturally).
- NEXT: dynamic capture on live device (Frida hook 0x74168 entry,
  one-shot + bt) -> name the lifecycle caller; if silent -> device-
  side POKE1187 synthesis (same IR probe as PC) as the one binary
  test (body runs? r0=0? sessions observed? burst?).

## 97. if-nez inversion: the paradox dissolves; POKE kills the app (2026-10-13)
- SUBAGENT S1 (PROVEN): Dalvik `if-nez` = branch iff !=0 (I had it
  BACKWARDS for hours). Corrected ST.onFrame: ready->doUpdate;
  doUpdate!=0 -> return 1 (continue, skip a()); doUpdate==0 -> a()
  -> doShutdown. doUpdate returns nativeUpdate verbatim (dexdump-
  verified, no transform). MyRenderer: onFrame 1 -> keep buffer;
  0 -> black clear (the black screen!). EXITING latches on r0=0.
- CORRECTED MODEL (fits EVERY boot, no paradox): no-poke bail
  r0=1 -> TRUE -> live (9762: 394 updates, 10 min). POKE body r0=0
  -> FALSE -> EXITING + a() -> deinit + suicide in seconds (12024,
  10463, 10821, 11125...). Convention: 1=keep going, 0=quit.
  Original body must return 1 (alive); ours returns 0 -> DIVERGENCE
  inside the body (POKE-forced-early with unbuilt content? +164=2?
  mocks?). Gate opens naturally only when content is READY.
- CONSEQUENCE: REMOVE POKE1187 from device (it guarantees death).
  Real wall = completion channel (sessions-succ posts nowhere,
  functors never fire, §62) + dynamic natives (BC0AC pump needs
  real RegisterNatives). Long-lived bail is the base again.
- METHOD (burned): verify branch opcode in dex FIRST (0x38/0x39),
  never trust "nez ~= zero" intuition; S1-style fresh-eyes audit
  beats my re-reading (I re-read it wrong 4 times).
- Frida attach now times out on live process (frida-helper sync
  fail; fastmem sighandler suspect or boot storm). Retry as needed.

## 98. +164=2 on device: logically LOADING PROVEN (2026-10-14)
- DIAG164 live (2fd8962 build): obj=10402ff8, +164=2, +1187=0,
  +168=scene, +172/+176=0, +216=1, scene188=1 entry, vec0 node
  vtable 0x669710 +16=1.0f (single loading node, PC-identical
  layout §55). 1:1 match with PC nopoke1 shape (flag/succ/+164=2).
- "Logically LOADING" is now MEASURED, not inferred. Remaining
  unobserved on device (all GL faked): pixels, splash advancement,
  builders. Next: completion channel (epilogue/drain subagents
  running) -> loginFinished -> burst; then real GLES for pixels.

## 96. History anchor (user-recounted 2026-10-13, keep for orientation)
- 4 days on the desktop harness past LOADING with zero menu progress
  (login ladder, builders, loop, manager, unwinder — all mapped, none
  delivering). Pivot to Android APK was the explicitly chosen exit
  (§75 correction -> §84 online-first packaging -> product boots on
  device same day). Standing rule from that pivot: device-via-Frida
  beats harness brute-force; PC stays a build+smoke forge, never the
  menu hunt. If a future thread proposes "one more harness trigger
  series" — re-read §§51-74 first (all shapes measured).
## 95. SAFEOPTS A/B: identical, ~20% faster on short combo (2026-10-13, PC)
- A (no_opt): 5.85s; B (all_safe_optimizations): 4.67s. Identical:
  54/54, svc=948, halts=53, updates/render r0=1 clean, same call
  shapes; heap-top delta 0x2A0 = known run drift, not signal.
- NOTE: short combo dominated by translation + PACEMS sleeps; steady-
  state gain larger. x64 only — A64+opts untested (A64 backend already
  bit us once, §80). Default stays no_opt; SAFEOPTS opt-in.
- NEXT: fastmem design (page-table 1:1 + sighandler + fallback), then
  A64 validation on device (opts+fastmem) withupdate-rate metric.
- RESULT (fmboot1, PC long combo + FASTMEM): 42s, 2 known worker
  spirals (time/apdrive escaping RETSUCC gate, §73 shape), film
  Rovio->...->LOADING end (user-confirmed normal end state; final
  white-frac 0.93 = bright LOADING art, not whiteout). Fastmem reaches
  the same stages as bring-up mode. A/B parity HOLDS long. Control
  (fmboot0, same combo no-fastmem) running to confirm.
