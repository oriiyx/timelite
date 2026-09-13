# 009: Time-range aggregation

Branch: feature/009-range-aggregation from main at 038057d, which includes
Feature 008. Initial tree and index clean; no stacked dependency.

## Desired state and plan

Add caller-owned count/minimum/maximum aggregation over a half-open range and
optional series filter. Use a borrowed cursor starting at the beginning and the
existing range reader for filtering, frame validation and ordered stopping.
Starting at the beginning also covers internally unordered legacy batches that
the seek last-record predicate could skip. Publish only a completed result.
Bounded local records and caller scratch; no ownership operations, writes,
allocation, new handle state, disk-format or backend changes.

Extend existing native/model tests for values, ranges, cursor independence,
validation, read faults, corruption and storage/ownership inactivity. Run
make check and then python3 tools/test.py run local. No new crash matrices.
Commit locally; container preflight deferred until a PR is requested.

## Actual changes and verification

Implemented `struct timelite_aggregate` and `timelite_batches_aggregate_range`.
A borrowed handle view is rewound and passed to the existing range reader.
A bounded 64-record local array receives validated matches; extrema use signed
comparisons without arithmetic, including INT64_MIN/INT64_MAX. File capacities
bound the record count below UINT64_MAX. Only the completed result is published.
No handle fields, ownership operations or persistent state changes were added.

Native tests cover empty database/interval/no matches, timestamp zero, equal
timestamps, signed extremes, half-open boundaries, series filters, multiple
installed segments plus WAL, checkpoint/reopen and midstream/END cursors.
Model tests inject failure at every aggregation read and corrupt an installed
header, installed payload and WAL payload, checking unchanged result and entire
handle. They cover NULL/invalid range/scratch, insufficient scratch, closed and
poisoned handles and legacy unordered data. All non-read model backend entry
points reject calls during aggregation, including writes, sync, truncate and
ownership operations. No new crash matrices or inventory entries.

README usage and public comments updated. AGENTS.md's deferred aggregation
statement became stale and was replaced; CLAUDE.md's project summary now mentions
Feature 009. No workflow or persistence rules changed.

- `make check`: PASS 10, SKIP 1 (Windows-only fault test), 8.6 seconds.
  Report: build/test-runs/run-20260913-143552-rqubri_t.
- `git diff --check`: passed.
- `python3 tools/test.py run local`: PASS 21, SKIP 2 (Windows-only fault
  test in native and sanitize), 32.9 seconds. Native, sanitizer and runner
  self-tests passed; native batch behavior exercised on supported storage.
  Report: build/test-runs/run-20260913-143621-19tnk8qa.
- Implementation/test source unchanged after passes; only this verification
  record was finalized. Stopped testing. Local commit only, no push/PR/merge.
  Container preflight deferred until a PR is requested. CI, Windows runtime,
  device and physical power-loss checks were not run.

## Limits

Queries scan candidate data, with no precomputed summaries or constant-time
promise. Combining series combines values; filter appropriately for units.
The exclusive upper endpoint cannot include UINT64_MAX. Persistence contracts
and platform limits remain unchanged. No sum, average, grouping or buckets.
