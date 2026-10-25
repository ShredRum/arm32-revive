# REVIEW.md — capital review of the shared core (started 2026-10-25)

The register of every finding of the review. Read this first when resuming.
AGENTS.md / JOURNAL.md / commit messages are treated as **hypotheses**: they were
written by small-context models and contain stale or wrong claims. Truth is the
code, the guest binary (disassembly), primary specs (NDK/Bionic headers, JNI,
GLES2, ARM AAPCS) and device behaviour.

## Rules

- A finding is fixed only after Claude verified it personally (code + spec or
  disassembly or a reproduction). Subagent reports are leads, not facts.
- One logical fix per commit, with a test where one is possible.
- Behaviour-changing fixes are batched for one device gate; the regression gate
  below must stay green after every batch.
- Preferred solution = the correct one, not the smallest. Ask the user only when
  two correct options genuinely differ in trade-offs.

## Regression gate (baseline 2026-10-25, HEAD 2cba59a)

Native harness (`host/build-native/harness <guest .so>`), guest SHA verified
against `games/<id>/profile.toml`:

| title | init | svc | halts | LOUD |
|---|---|---|---|---|
| rio | 54/55 | 949 | 53 | 0 |
| seasons | 142/143 | 3073 | 142 | 3 (build-id pin noise, R-001) |
| space | 67/68 | 1330 | 67 | 3 (same) |
| star-wars | 96/97 | 2320 | 96 | 3 (same) |
| star-wars-ii | 143/144 | 4457 | 143 | 3 (same) |
| star-wars-ii-amz | 143/144 | 4457 | 143 | 3 (same) |

Also: `fmt_test` ALL OK (shipping + `--diag`), `zip_test`, `cbnz_test`,
`smoke_a32`; Seasons golden level (85-line state + 46 JSON fields, see
games/seasons/JOURNAL.md); device gate on the sandbox
(`ssh -p 8022 u0_a41@192.168.1.202`, markers HEAPCORRUPT/HEAPASSERT/CHUNKD/
USAGE_ERROR/LOUD/STUCK/FATAL = 0, files/death.log 0 B, process alive).

## Areas and waves

| id | area | scope | wave | status |
|---|---|---|---|---|
| A | libc core | dispatch.cpp OnSvc/ExpandPrintf/math/CallImport malloc..poll; math_thunks.inc, bsd_random.h | 1 | |
| B | stdio/files/sockets/assets/time/props | dispatch.cpp CallImport pthread_once..end, InetThunk, MapGuestPath | 1 | |
| C | memory/heap/threads/sync/kuser | dispatch.cpp 1..1103, Pthread*, kuser, syscall, exidx; live_heap.h, guest_memory.h, layout.h | 1 | |
| D | JNI bridge + app glue | dispatch.cpp JNI part; apphost.cpp, apphost_gen.inc, gen_apphost.py | 1 | |
| E | GL | realgl.cpp + gl* in dispatch.cpp | 2 | |
| F | loader + JIT callbacks + probes | elf_loader.cpp, GuestCallbacks::*, harness.cpp | 2 | |
| G | crutches / magic numbers / HARNESS_* / dead code | whole tree | 2 | |
| H | build, packaging, scripts, smali edits | scripts/, CMakeLists, prepare-tree | 2 | |

## Findings

Severity: S1 = crash/corruption/wrong semantics on a live path; S2 = latent bug
(reachable, not observed) or wrong ABI on an unused path; S3 = crutch/magic
number/misleading doc; S4 = cosmetic.
Status: open / confirmed / refuted / fixed (commit) / wontfix (reason).

| id | sev | where | finding | evidence | status |
|---|---|---|---|---|---|
| R-001 | S4 | elf_loader.h:134 CheckGuestBuildId; harness.cpp:687 | harness-only: non-Rio guests have no GNU build-id note, harness does not verify the SHA like apphost does (apphost.cpp:1484 passes identity_verified), so it prints LOUD. Bigger question for G: the shared core pins Rio VAs/hooks at all | readelf -n: no build-id in the 5 non-Rio guests; device path prints a plain line | confirmed |
| R-002 | S2 | dispatch.cpp:4114,10071,10080,10086 | printf `%u` given size_t/unsigned long | clangd -Wformat | open |
| R-003 | S2 | JNI mock refs | mock jstrings never freed (~86 KB/h Rio); needs real local/global ref lifetimes | AGENTS §145 | open |
