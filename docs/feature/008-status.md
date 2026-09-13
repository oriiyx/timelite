# 008: Database status

Branch: feature/008-status from main, which includes Feature 007. Initial tree
and index were clean; no stacked dependency is needed.

## Desired state and plan

Add a caller-owned status struct and metadata-only getter for committed,
installed and pending batches, installed segments, committed WAL bytes,
installed segment bytes and last committed timestamp. Validate arguments and
handle readiness before touching output. Preserve the entire handle and cursor.
Byte counts describe logical committed data, excluding orphan bytes and physical
allocation; existing capacity limits and manual checkpoint behavior remain.
No I/O, scratch, allocation, scans, handle state, disk or backend changes.

Extend existing native/model tests for lifecycle values, timestamp zero,
argument/closed/poison errors, unchanged output, cursor and storage inactivity.
Run make check, then python3 tools/test.py run local. Container preflight is
deferred until a PR is requested. Commit locally; no push, PR or merge.

## Actual changes and verification

Implemented `struct timelite_batches_status` and
`timelite_batches_get_status` with all seven requested uint64_t fields. Existing
readiness validation runs before output assignments; values come directly from
handle metadata (installed bytes subtract the 160-byte logical prefix, including
for legacy handles). No handle fields, persistence or backend changes.
Public comments and a short README usage example describe the contract.
AGENTS.md and CLAUDE.md reviewed; neither became stale.

Existing native tests check empty, installed and reopened mixed states. The model
checks append, timestamp zero, checkpoint, reopen, closed/NULL/poison errors,
unchanged error output and byte-for-byte unchanged handles, including the cursor.
Every model storage entry point rejects calls while the getter is being tested.
No new crash matrices were added.

Verification:
- Initial `make check`: native passed; model failed because the new test left a
  changed timestamp in shared test input. Fixed by restoring the original value.
- Final `make check`: PASS 10, SKIP 1 (Windows-only fault test), 8.8 seconds.
  Report: build/test-runs/run-20260913-113411-34hccflw.
- `python3 tools/test.py run local`: PASS 21, SKIP 2 (Windows-only fault test
  in native and sanitize), 36.5 seconds. Native, sanitizer and runner self-tests
  passed. Report: build/test-runs/run-20260913-113424-t4vekex9.
- `git diff --check`: passed. Implementation/test source unchanged after passes;
  only this verification record was completed afterward.

Remaining limits: logical counts only; manual checkpoint and existing capacities
are unchanged. Container preflight is deferred until a PR is requested. CI,
Windows runtime, device and physical power-loss checks were not run. Local
commit only; no push, PR or merge.
