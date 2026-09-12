# 005: Repeatable testing suite

Status: implemented; verification results recorded below. Not committed.

Feature 005 replaces the previously scheduled checkpointing work. Checkpointing,
main storage and WAL reclamation remain deferred with no number reserved.
Feature notes 003 and 004 keep their historical "feature 005" wording for
checkpointing; note 004 carries a scheduling annotation.

## Desired state

Verification is routine: register a test once, run a documented command, and
get a trustworthy result without inventing shell commands, rebuilding
environments, switching compiler flags by hand or interpreting large logs.
Production code and behavioral/fault/model tests stay in C99. Orchestration is
a small Python 3 standard-library runner. Make remains the ordinary build.
Docker gives reproducible Linux x86-64, x86-32 and MinGW cross toolchains;
native GitHub runners cover macOS and Windows runtime. No storage redesign,
weakened durability checks, fuzzing, benchmarks or device lab.

## Baseline and Git

Fetched origin on 2026-09-12 (a later fetch in this session failed on SSH
access, but the local `origin/main` already contained the baseline). Feature
004 commit `a576c5e` is both local `main` and `origin/main`, so
`codex/005-testing-suite` branches from it without a stacked base. The
previous agent session had staged the first draft of this note, the shared
test headers, `tools/test.py` and `tools/inventory.json` and left unstaged
test edits; all were preserved and continued. The draft runner did not parse
(a Python syntax error) and was rewritten with the same design. Nothing was
committed, pushed or merged.

## Implemented design

### Inventory (tools/inventory.json)

One JSON list shared by the runner and CI. Each entry has `name`, `source`,
`backend` (`library` links the static library built once per configuration;
`native` compiles the test with the one native backend; `fake` compiles it with
`timelite.c` only; `none` compiles the test alone), optional `defines`,
`platforms`, a `coverage` sentence, optional `execution` (`probe`, `batch`,
`batch-example`) and optional expected `stdout`. Adding a C test is one entry;
adding a case to an existing test changes only the C file. The Makefile and the
workflow contain no test list.

Registered: `basic` and `batches` (public static-library consumers, output
checked), `storage_probe`, `file_io`, `file_io_fault`, `file_io_windows_fault`,
`provision_fault`, `lifecycle`, `lifecycle_fault`, `batch`, `batch_model`.

### Runner (tools/test.py)

- Actions: `list`, `doctor`, `prepare`, `run PROFILE|GROUP` with `--test`,
  `--jobs` (1..8, default 2), `--timeout` (per command, default 120 s),
  `--test-root`, `--output`. Every command is an argument array; no shell.
- Profiles: `native`, `sanitize` (ASan + UBSan), `runner` (unit tests of the
  runner), `linux`, `linux32`, `windows-cross` (compile only), `windows-native`.
  Groups: `local` = native + sanitize + runner; `preflight` = local + linux +
  linux32 + windows-cross. Missing tools produce BLOCKED rows, never removal.
- Source identity: one bounded snapshot per invocation (allowlisted source,
  tests, examples, tools, docs and workflow files; symlinks refused; 16 MiB
  limit; `.git`, `build/` and unrelated files excluded). Every profile builds
  from the snapshot. The digest, Git commit and dirty list are recorded; the
  working tree is digested again at the end and a change adds a failing
  `source` row marked stale. Containers receive the snapshot read-only and
  their report digest must match.
- Isolation: each run has a unique directory `build/test-runs/run-<time>-<id>`
  with one folder per profile for objects, binaries and one log per command.
  Configurations never share objects. Storage tests get a private directory
  each (`TIMELITE_TEST_ROOT`, `TMPDIR`, `TMP`, `TEMP`), under the run directory
  or under a private child of `--test-root`.
- Statuses: PASS, FAIL, SKIP (required or optional), BLOCKED, NOT RUN with
  reasons. A run passes only with at least one PASS and no failing row.
  Empty selection, timeout, missing executable, required skip, cancellation
  and staleness all fail. Exit codes: 0 pass, 77 provisioning unsupported,
  78 required group skipped; anything else fails.
- Coverage separation: the C `storage_probe` decides storage capability; no
  Python filesystem guess. `batch` on supported storage records "native batch
  behavior exercised"; exit 77 is the verified contract on Windows, an
  optional skip labelled "NOT RUN" in containers (overlay/shared storage is
  not durability evidence) and BLOCKED in `native`/`sanitize`, which require
  supported storage or an explicit `--test-root`. The batch example runs only
  with supported storage and its output is compared with the inventory.
- Ownership: children run in their own process group and are killed on
  timeout or Ctrl-C; the named container is removed with `docker rm -f` in a
  `finally` block. Reports are still written after cancellation.
- Reports: terminal summary, `report.json` (source digest, Git, host and
  target architecture from compiler macros, compiler version, image identity
  and toolchain versions, emulation flag, exact commands, logs, timings, exit
  codes, reproduction command per row) and `junit.xml`.
- Containers: `--pull=never --network=none --platform=linux/amd64`, read-only
  source mount, writable run directory, non-root user id, no privileges or
  devices. The image tag is derived from the Dockerfile hash.

### Toolchain image (tools/docker/Dockerfile)

`debian:bookworm-slim` pinned by digest
`sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171`
with gcc, gcc-multilib, mingw-w64 x86-64 and python3. Package versions come
from the Debian repositories at `prepare` time and are recorded in
`/etc/timelite-toolchain.txt`, printed by `prepare` and copied into each
container report. Recorded on 2026-09-12: gcc 12.2.0-14+deb12u1
(x86_64-linux-gnu), x86_64-w64-mingw32-gcc 12-win32 (package
12.2.0-14+25.2), binutils 2.40-2, glibc dev 2.36-9+deb12u14, Python 3.11.2.
The digest pins the base; apt packages are recorded, not pinned, so full
reproducibility is not claimed from the tag alone.

### Make, CI and tests

- `make all` builds the library and examples; `make check` runs `make all`
  then `python3 tools/test.py run native`, passing CC/AR/CPPFLAGS/CFLAGS/
  LDFLAGS/LDLIBS through. `build/flags` records the toolchain and flags at
  parse time and removes Make's own outputs when they change (Apple GNU Make
  3.81 compares timestamps at one-second granularity, so a stamp alone was
  not enough).
- GitHub Actions: `local` on ubuntu-latest and macos-latest plus `make check`;
  `prepare` then `linux`, `linux32`, `windows-cross` on ubuntu-latest;
  `windows-native` on windows-2022. All jobs have timeouts and upload
  `build/test-runs` with `if: always()`.
- Tests: `tests/test_assert.h` replaces `<assert.h>` in assert-based tests so
  checks survive NDEBUG and print file, line, function, case name and
  boundary; loops in the model, batch, lifecycle-fault and provisioning tests
  set `TEST_CASE`. `tests/posix_test_paths.h` creates private directories under
  `TIMELITE_TEST_ROOT` (default /tmp). `tests/storage_probe.c` is new.
  `batch_test` returns 77 on ENOTSUP and asserts ENOTSUP on Windows.
  `file_io_test` returns 78 when the >4 GiB sparse group is skipped. The batch
  example now runs as a permanent test in owned storage with checked output.
- Runner self-tests (`tools/test_runner.py`, 15 cases): success, failure and
  missing executable; timeout kill and reap; cancellation; pass semantics
  (empty, optional-only and required skips); snapshot exclusions and digest
  change; passing/failing/wrong-output fixtures with report and JUnit
  accuracy; single-test selection; empty selection; required versus optional
  skips including selecting an inapplicable test; missing compiler; timeout as
  failure; source change marking results stale; native versus sanitize
  isolation; storage probe semantics for required, contract and optional
  policies; private child of an explicit test root. Fixture repositories live
  in directories containing spaces.

## Verification

Host: macOS 26.6.2, Apple Silicon (arm64), Apple clang 21.0.0, GNU Make 3.81,
Python 3.14.7, Docker 29.4.0 (OrbStack; containers run linux/amd64 under
emulation and the reports say `emulated: true`).

Commands actually run during implementation, all on this host:

- Direct strict builds and runs of every test program: all exit 0
  (probe reports supported provisioning on APFS).
- `python3 tools/test.py run native --jobs 4`: PASS 10, SKIP 1 (optional,
  Windows-only fault test), batch coverage "native batch behavior exercised".
- `python3 tools/test.py run sanitize --jobs 4`: PASS 10, SKIP 1.
- `python3 -m unittest discover -s tools -p test_runner.py`: 15 tests OK.
- `python3 tools/test.py doctor`: compiler and archiver PASS; Docker checks
  fail inside the sandbox used for most commands and pass outside it.
- `python3 tools/test.py prepare`: PASS, image `timelite-tests:389ffa5e6fa1c9d8`,
  toolchain versions as recorded above.
- `python3 tools/test.py run linux --jobs 4`: PASS 7, SKIP 4 (Windows-only
  test; probe, batch and batch example optional: the bind mount is virtiofs,
  provisioning unsupported, native batch NOT RUN).
- `python3 tools/test.py run linux32 --jobs 4`: same shape; target recorded
  as "Linux x86 32-bit, 4-byte pointers".
- `python3 tools/test.py run windows-cross --jobs 4`: PASS 9 cross-builds,
  SKIP 2 (POSIX-only fault tests); target "Windows x86-64, 8-byte pointers".
- `python3 tools/test.py run linux32 --test batch`: only an optional skip,
  therefore FAIL "no test ran to completion" after the pass rule was
  tightened (previously it passed with nothing exercised).
- `make check` after `make clean`: PASS. `make CC=clang CFLAGS=-O2 all`
  followed immediately by `make all` rebuilt everything both ways without
  `make clean`; a third `make all` did nothing.
- Failing fixture: the runner self-tests build an intentionally failing
  fixture program (exit 5), a wrong-output fixture and a hanging fixture and
  assert the suite reports FAIL with exit code 1, without touching production
  code. Filtered runs (`--test`) and native versus sanitize isolation are
  asserted the same way.

### Final results

On the finished tree (all files except this results section, which was
written afterwards; source digest `b391b924c5ba7c5d…`, commit `a576c5e`
dirty):

- `make check` after removing build/: PASS 10, SKIP 1 (optional).
- `python3 tools/test.py run preflight --jobs 4`: PASS 44, SKIP 12, all
  skips optional, not stale, 45.5 s. Per profile: native and sanitize PASS 10
  each with batch coverage "native batch behavior exercised" (macOS ARM64,
  APFS, provisioning supported); runner PASS (15 unit tests); linux and
  linux32 PASS 7 each with probe, batch and batch example recorded as optional
  NOT RUN (virtiofs mount, provisioning unsupported; targets "Linux x86-64"
  and "Linux x86 32-bit, 4-byte pointers", `emulated: true`); windows-cross
  PASS 9 cross-builds with target "Windows x86-64, 8-byte pointers".
- A first attempt at this final run was aborted by a concurrent `make clean`
  that removed the run's snapshot; the runner stopped with BLOCKED and exit
  2. The run above is the clean repeat.

Not run: GitHub Actions (the workflow was refactored but no CI run has
executed; do not treat it as passing), `windows-native` (no Windows host
here), Linux x86-64 or x86-32 execution on real hardware (only emulated
containers), ARM32 or device builds, and physical power-cut testing.

## Known gaps and remaining work

- Native batch durability coverage inside containers depends on the mount's
  filesystem. On the macOS development host it is NOT RUN; on an ext4 CI runner
  the bind mount is expected to be supported, which the reports will show.
  `--test-root` on a supported filesystem is the explicit alternative.
- The Docker image pins the base by digest but installs apt packages from the
  live repositories; versions are recorded, not frozen.
- `windows-native` relies on `clang` and `llvm-ar` on PATH; the first CI run
  will confirm the windows-2022 image provides both.
- Container cleanup after Ctrl-C is implemented but not covered by a unit
  test (it needs Docker); child-process cancellation is.
- The `runner` profile compiles fixture programs with the host compiler and
  takes about fifteen seconds; it is part of `local`, not of `make check`.
- Checkpointing, WAL reclamation and device testing remain deferred.
