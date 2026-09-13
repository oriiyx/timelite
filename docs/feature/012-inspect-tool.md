# 012: Offline inspection and verification tool

Status: implemented; `make check` and `python3 tools/test.py run local` pass
locally as recorded below. Work is on the
user-selected `main` checkout; the user owns every Git mutation. The working
tree and index were clean at the start.

## Desired state

An operator can check a Timelite database/WAL pair from the shell after a
crash, without writing C: see what is on disk, whether it is coherent, and
what the library's recovery would do. The database feature set is unchanged.

## Decisions recorded before coding

1. One small C99 program, `tools/inspect/inspect.c`, built by Make as
   `build/timelite-inspect` and compiled by the test runner through its
   inventory entry. Fixed buffers only: no allocation, threads or
   dependencies beyond the C standard library and the public header.
2. Two modes with an explicit difference:
   - `verify DATABASE WAL` is read-only. It opens both files with standard C
     stdio, never through the library, and parses the byte layouts documented
     in features 004, 006, 007 and 010. It never writes, truncates, syncs or
     creates anything. It reports what it read and predicts what recovery
     would do, without doing it.
   - `status DATABASE WAL` uses only the public API (`timelite_batches_open`
     with `TIMELITE_OPEN_EXISTING`, `timelite_batches_get_status`,
     `timelite_batches_rewind`, `timelite_batches_next`, `timelite_batches_seek`,
     `timelite_batches_next_range`, `timelite_batches_close`). Opening performs
     the library's normal recovery (incomplete WAL suffix truncation,
     interrupted reclaim, unfinished retention), and the output header says
     so.
3. No new public API. Everything `verify` needs that the public API cannot
   give (slot contents, offsets, phases, checksums) is parsed from the file.
   No getter is added to the library.
4. Format knowledge stays in `timelite.c`. The tool defines the constants it
   needs itself, each with a comment naming the source feature note, and the
   inventory test fails when they diverge: every pair the real library
   creates or recovers must verify with zero inconsistencies, the documented
   golden manifest bytes of features 006 and 007 must parse, and the tool's
   predicted post-recovery status must equal the library's status on the same
   bytes. A drifted magic, offset, CRC polynomial or validity rule breaks
   one of these assertions.
5. Exit codes: 0 coherent, 1 inconsistency found, 2 usage or I/O error.
   Recoverable crash artifacts that the library resolves without losing a
   committed batch (an incomplete WAL suffix, a stale WAL after an
   interrupted reclaim, orphan bytes from an interrupted install, an
   interrupted retention phase) are coherent: they are reported under
   `recovery.*` and `*_trailing_bytes` keys and exit 0. Damage and fail-closed
   states (bad checksums, invalid nonzero manifest slots, sequence or link
   errors, ambiguous short framing, v1 files, mismatched pairs) exit 1.
6. Output is plain text, one fact per line, `key=value`, stable for grep.
   Indexed facts use dotted keys (`segment.1.offset=160`,
   `inconsistency.1.offset=...`). Nothing is printed that was not read from
   the file or derived from it; absent facts are omitted rather than
   invented.
7. The tool reports; it never fixes. No repair or rewrite mode, no automatic
   policy, no format change, no new query feature.
8. Windows: `verify` builds and runs there because it uses stdio only.
   `status` reports the library's `ENOTSUP` exactly as the library returns it
   and exits 2. Exactly one native backend is linked.

Why: a torn header or lost manifest can prevent opening committed data until
someone looks at the bytes (feature 004 explicitly deferred a repair tool and
this feature keeps that decision). The operator needs a description, not a
mutation, and the description must match what the library will do, which is
why the tool mirrors the open logic of `timelite.c` and the tests compare its
prediction against the real recovery.

## Scope

- `tools/inspect/inspect.c`, Make target `build/timelite-inspect`.
- `tests/inspect_test.c`, one inventory entry named `inspect`.
- README section "Inspecting a database after a crash" and layout list.
- AGENTS.md and CLAUDE.md summaries.

Out of scope: repair, rewrite, migration, new database APIs, format changes,
automatic policies, allocation, dependencies, new query features.

## Planned verification

`make check` while iterating; `python3 tools/test.py run local` once ready.
Container preflight is deferred until a pull request is requested. Windows
runtime, CI, devices and physical power loss are not claimed.

## Actual changes

- `tools/inspect/inspect.c`: the program. `verify` streams each file once
  for its physical size, then reads headers, both manifest slots, every
  segment header and every frame in the logical extent, and the WAL, with
  the same validity rules as `timelite_batches_open` (slot parity, highest
  revision, orphan rule, stale WAL, incomplete suffix). It prints the facts
  listed in the README and `predicted.*` lines whose keys match the fields of
  `timelite_batches_status`. `status` prints the same keys without the
  prefix after opening, plus `batch=... series=... us=... value=...` lines
  with `--dump`, optionally filtered by `--from-us`, `--until-us` and
  `--series`. `main` is excluded with `TIMELITE_INSPECT_NO_MAIN` so the test
  compiles the program into itself and captures its output.
- `tests/inspect_test.c` (inventory `inspect`, backend `fake`, execution
  `batch`): includes exactly one native backend with renamed entry points so
  the library's write, sync and truncate calls pass through counting wrappers
  that can fail before operation N. Synthetic fixtures (documented golden
  creation image, v1 file, second identity, damaged slot) run on every
  platform, including Windows and the containers where provisioning is
  unsupported. On supported storage the test creates real pairs with the
  library: fresh, pending WAL, after checkpoint, incomplete WAL suffix,
  ambiguous short suffix, damaged WAL frame, damaged installed frame, damaged
  newest and older manifest slot, mismatched pair, and retention interrupted
  before every write/sync/truncate (the observed PREPARE, TAIL and CLEANUP
  phases are asserted). For each fixture it asserts the exit code and the
  required lines, that both files are byte-identical before and after
  `verify`, and that `status` on the same bytes reports the open result and
  every status field that `verify` predicted, then verifies the recovered
  pair again. Where provisioning is unsupported it asserts `status` reports
  `ENOTSUP` with exit 2 and returns 77.
- `Makefile`: `build/timelite-inspect` target, part of `all` and the flag
  change cleanup list.
- `tools/inventory.json`: one entry. `tools/test.py`: the source snapshot
  allowlist now copies `.c` and `.h` files under `tools/` so the runner can
  compile the tool; no other runner change.
- README: new section and layout list; AGENTS.md and CLAUDE.md summaries.

## Verification results

Host: macOS (Darwin 25.6.0) on Apple Silicon, Apple Clang, Python 3.14. All
results below are local; CI, Windows runtime, Linux containers, ARM32,
deployment devices and physical power loss were not run and are not claimed.
Container preflight is deferred until a pull request is requested.

Commands actually run, in order:

- `cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. -c tools/inspect/inspect.c`
  while iterating: one unused helper removed, then clean.
- Direct build and run of `tests/inspect_test.c` with `timelite.c` while
  iterating. Three test-side or tool-side mistakes were caught and fixed, none
  in the library: the test's byte-identity check overwrote its own saved
  fixture buffers (so a later case saw a still-damaged WAL); the tool counted
  a well-formed older slot whose extent a later retention phase had truncated
  as damage (it is now reported as `superseded`, which the library also
  ignores silently); and the runner's source snapshot did not copy `.c` files
  under `tools/`, so the test could not include the tool (the allowlist now
  includes `.c` and `.h` there).
- `make check`: first run FAIL only in `inspect` (the snapshot omission
  above); second run PASS 12, SKIP 1 (optional Windows-only fault test).
- `python3 tools/test.py run local` on the pre-refactor tool: PASS 25, SKIP 2
  (optional), 37.2 s, report `build/test-runs/run-20260913-154618-39g3lzz3`.
- The tool's `verify` was then split from one 570-line function into one
  function per stage of the library's open (headers, manifests, segments,
  WAL, summary) sharing one state struct; behaviour unchanged. Direct native
  and AddressSanitizer/UndefinedBehaviorSanitizer runs of the test passed.
- Final `make check` and `python3 tools/test.py run local` on the unchanged
  final source: `make check` PASS 12, SKIP 1 (optional), 12.7 s,
  report `build/test-runs/run-20260913-155042-92y775is`; `run local` PASS 25,
  SKIP 2 (optional), 37.1 s, report `build/test-runs/run-20260913-155054-jacy0lfj`,
  not stale. `inspect` passed in native (3.9 s) and sanitize (3.2 s). Only this
  note was edited afterwards; testing stopped under the stopping rule.
- `git diff --check`: passed.

The `inspect` test on this host: usage errors; synthetic fresh pair (documented
creation image), v1 file as database and as WAL, mismatched synthetic pair,
damaged slot, damaged WAL header; then with the real library: fresh pair (its
generation 0 manifest equals the documented golden bytes), pending WAL, after
checkpoint with `--dump` and filtered dump, incomplete WAL suffix, ambiguous
1..31-byte suffix, damaged committed WAL frame, damaged installed frame,
damaged newest slot with pending and with empty WAL, damaged older slot,
mismatched real pair, stale WAL after a checkpoint interrupted before its
truncate, and retention interrupted before each of its 16 write/sync/truncate
operations (PREPARE, TAIL and CLEANUP were all observed). For every fixture
the test asserts the exit code and the named lines, byte-identical files
after `verify`, and that `status` reports the open result and all seven
status fields `verify` predicted; recovered pairs verify clean afterwards.
Runtime about 4 s per profile.

## Drift found between the feature notes and the implementation

- Feature 006, "Recovery and open" rule 2, says a valid manifest in the wrong
  slot for its generation fails closed. `read_manifests` in timelite.c skips
  such a slot silently (`continue`) and chooses among the remaining valid
  slots. The tool mirrors the code and reports the skipped slot as an
  inconsistency. The note's wording should follow the code (a documentation
  defect; no library change was made here).
- Feature 006 also says two valid manifests with equal generations fail
  closed; with parity selection they cannot both be accepted, so the code has
  no such branch. Harmless, but the sentence describes a state that cannot
  arise.
- No note states which error a batch open returns for a v1 lifecycle file.
  The code returns `TIMELITE_UNSUPPORTED_VERSION` (version 1 is a nonzero
  unknown version to the pair-header check), not `TIMELITE_INVALID_DATABASE`.
  README and the header say only "rejects v1". The tool and test record the
  actual code.
- Every byte offset, magic, marker, phase code, CRC and validity rule in
  features 004, 006, 007 and 010 matched what the tool needed to accept every
  pair the library produced; no layout drift was found.

No library defect was exposed: every recovery the library performed matched
the prediction made from the bytes.

## Remaining limits

- The tool predicts recovery by mirroring the open logic of timelite.c; a
  future format change must be mirrored by hand, and the `inspect` test is
  what detects a mismatch.
- `verify` reads with stdio and `fseek(long)`. Physical sizes are counted by
  streaming, but offsets beyond `LONG_MAX` are treated as end of file; valid
  layouts never need them (1 GiB capacity). A damaged manifest that points
  past 2 GiB on a 32-bit build is reported as short rather than read.
- Paths are passed to `fopen` as given; non-ASCII paths on Windows are not
  converted, so `verify` may fail to open them there. `status` uses the
  library's UTF-8 handling but returns `ENOTSUP` on Windows.
- At most 32 inconsistencies are listed individually; the total is always
  printed.
- The half-open `--until-us` cannot include `UINT64_MAX`, as for the
  library's range API.
- `verify` cannot prove what the storage will return after a power cut; it
  describes the bytes the operating system shows now.
- No repair, rewrite or migration is offered, by design.
- Windows runtime, containers, CI, ARM32 and devices have not run this test;
  the Windows branch (synthetic `verify`, `status` reporting `ENOTSUP`, exit
  77) is compiled and reasoned about only.
