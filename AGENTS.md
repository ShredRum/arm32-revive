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
