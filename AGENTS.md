# Instructions for coding agents

## Project

Timelite is a small embedded time-series database project written in C99.
The code implements v1 lifecycle plus a separate v2 durable sensor batch API,
with a paired WAL, recovery, sequential and time-range reading (feature 007),
globally ordered append, indexed segment seek and explicit checkpointing
(feature 006): committed WAL batches are installed into the main database file
as immutable segments behind a two-slot generation manifest, and the WAL is
truncated only after that install is durable. The WAL is capped at 64 MiB and
the main file at 1 GiB; checkpoint is manual and never automatic. Feature 005 is
the repeatable testing suite. Feature 010 adds explicit whole-segment retention
through durable tail/front
copies and recovery-phase manifests; sequence and timestamp high-water marks
survive expiration. General compaction and per-series indexes remain deferred. Feature 009 adds scanning
time-range count/minimum/maximum aggregation without changing the read cursor.
Batch operations use caller-owned scratch, integer records and serialized ownership.
Durable provisioning is implemented for selected local Linux/macOS filesystems;
Windows batch provisioning explicitly returns ENOTSUP. v1 creation retains its
original file-sync-only contract. No automatic format migration is provided.
Read README.md and the relevant docs/feature notes before editing. Earlier research
was removed from main; historical proposals are not automatically accepted
requirements. These instructions describe the current direction.

Target devices include x86 and ARM32. Confirm the exact CPU, OS, and ABI before
claiming support. A desktop build does not prove that the device build works.
Rust is excluded as the core implementation language.

## Code style

Use simple, explicit C99 in the style requested by the user:

- Use plain structs, functions, loops, and explicit control flow.
- Use four spaces, braces on their own lines, and snake_case names.
- Prefix public names with timelite_. Keep private functions static.
- Use clear names and short comments that explain intent or a non-obvious rule.
  Write for a junior developer. Avoid jargon, clever wording, and comments that
  only repeat the code.
- Keep internal file operations in file_io.c (POSIX), file_io_windows.c (Windows),
  and file_io.h, separate from timelite.h. Linux is provisional and macOS is the
  development target. Windows desktop initially targets x64 Clang with the Windows
  SDK; runtime validation is pending. Preserve the Windows public-library smoke
  build and backend tests. Keep UTF-8 paths, bounded conversion, explicit sharing,
  errno mappings, and close ownership documented.
- Keep the core in timelite.c and the public declarations in timelite.h while
  that remains easy to understand. Split files when there is a concrete need.
- Add no third-party dependency without a concrete reason and agreement on scope.
  Standard C headers and OS headers are fine. Do not recreate them to avoid includes.
- Prefer caller-owned memory and explicit buffer sizes. Do not add hidden
  allocations, global mutable state, background threads, or a general framework.
- Add abstractions only for an actual need. Keep OS calls behind a small storage
  boundary when storage is introduced; do not build unused platform layers now.
- Use fixed-width fields and explicit byte encoding for persistent data. Never
  write native structs directly to disk. Check lengths, overflow, and errors.
- Never assume an in-place overwrite is atomic. Install new persistent state
  by writing and syncing it in an unreferenced place first, then switching a
  checksummed generation record that lives in the slot not currently in use.
  Reclaim redundant data (for example WAL frames) only after that switch is
  synced, and make recovery finish any interrupted reclaim.
- Do not claim crash safety, durability, or thread safety without a defined
  contract and tests. A completed write is not necessarily a durable commit.

## Git workflow

- Work on a feature branch, normally codex/NNN-short-name. Never develop, commit,
  or push directly on main. Changes reach main through a GitHub pull request.
- Check the branch and working tree before editing. Preserve existing user edits
  and staged changes. Do not include unrelated work in a commit.
- Do not commit or push unless the user requests it. When publishing is requested,
  push the feature branch and open a pull request targeting main. Never force-push
  main or bypass branch protection. Merge only when authorized and checks pass.

## Feature notes and instruction maintenance

- Before implementing a feature, create docs/feature/NNN-short-name.md using the
  next unused number. Continue its note for follow-up work on the same feature.
- Record the desired state, scope, planned approach, actual changes, verification
  commands/results, and any remaining work or limits.
- Update the note as the work changes and before handing it back to the user.
  Clearly separate planned work from completed work and unrun checks from passes.
- Review AGENTS.md and CLAUDE.md during each feature. Update them whenever an
  accepted decision changes the workflow, style, architecture, or build process.
  Keep shared rules here and keep Claude's entry point consistent with them.
- Update README.md when setup or usage changes. Do not leave stale instructions.

## Build and verification

- Standard workflow: `make check` is the fast native entry point. It builds the
  library and examples with Make, then runs `python3 tools/test.py run native`.
  Before handing work back, run `python3 tools/test.py run local` (native +
  sanitize + runner self-tests). Before a pull request, prepare the Docker image
  once with `python3 tools/test.py prepare` and run
  `python3 tools/test.py run preflight` (local + linux + linux32 + windows-cross).
  `python3 tools/test.py doctor` explains missing tools; `list` shows profiles
  and the inventory. See README.md "Testing" for statuses and artifacts.
- Tests are registered once in tools/inventory.json, which the runner and CI
  share. Adding a C test is one inventory entry plus the source file; adding a
  case to an existing test needs no runner change. Do not add test lists to the
  Makefile or workflows.
- Test programs return 0 on success, 77 when durable provisioning is
  unsupported on the storage (the expected contract on Windows) and 78 when a
  required group could not run (the >4 GiB sparse file group and the 1 GiB
  sparse capacity fixture). Any other exit is a failure. Use test_assert.h
  (never <assert.h>) so checks survive NDEBUG and name the case and boundary.
- Every run gets a unique directory under build/test-runs with a source
  snapshot, per-command logs, report.json and junit.xml. Results are marked
  stale if the working tree changes during the run. A container run on an
  overlay or shared filesystem records native batch coverage as NOT RUN; it is
  not durability evidence. Model tests do not prove physical power-loss safety.
- Stopping rule: once the required checks pass for the relevant source and
  that source is unchanged, stop testing. Rerun only for a source change, a
  failure, or a concrete unresolved coverage gap. When an ad hoc command
  provides reusable coverage, turn it into an inventory test or runner
  operation instead of repeating it by hand.
- Keep builds warning-free under the strict C99 flags; the runner always adds
  them. CC, AR, CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS override the host
  toolchain for Make and for the native/sanitize/windows-native profiles. Make
  rebuilds automatically when flags change; `make clean` also removes
  build/test-runs.
- Direct public-library builds must link exactly one native file backend.
- Add focused tests as real behavior appears. Storage changes need failure and
  recovery tests; do not add tests that merely duplicate trivial implementation.
- GitHub Actions runs the same profiles: `local` on Linux and macOS, the three
  container profiles on Linux x86-64 runners (no emulation there),
  and `windows-native` on Windows Server 2022 x64 with Clang and the SDK.
  Reports are uploaded even on failure. Report local results separately from
  CI and device results. Never claim unrun checks passed.
- Do not add database features as part of unrelated setup or documentation work.
