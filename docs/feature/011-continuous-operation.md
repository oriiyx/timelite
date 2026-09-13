# Feature 011: Continuous operation and release hardening

## Desired state

Demonstrate that an application can continuously append sensor batches, inspect
status, explicitly checkpoint, query retained history and explicitly expire
whole segments while preserving the documented Timelite v0.1 contracts. Keep
retention policy, scheduling and export decisions in the application.

## Scope

- Add a small C99 continuous-operation example using the existing public API.
- Exercise a deterministic bounded append/checkpoint/retention/reopen/query
  workload against a simple reference model.
- Cover equal timestamps, cutoffs spanning segments, pending WAL data, status
  counts, the preserved timestamp floor and `TIMELITE_END`.
- Review operation interactions and add focused regression coverage for any
  concrete defects found, reusing the existing failure-injection coverage.
- Align public comments and README guidance for the v0.1 contract, including
  temporary retention-copy capacity and evidence boundaries.

No automatic policy, allocation, scheduling, background work, compaction,
indexes, query features, dependency or release publication is in scope. No new
database API is planned.

## Planned approach

1. Map the current append, status, checkpoint, retention, reopen and query
   contracts and the existing native/model test helpers.
2. Add the application example and register it in the shared test inventory and
   Make build.
3. Extend an existing test with a deterministic reference-model workload and
   focused assertions for the required interaction boundaries.
4. Fix only concrete integration defects demonstrated by the workload.
5. Update public comments and README, then run `make check` while iterating and
   `python3 tools/test.py run local` once the source is ready.

## Actual changes

- Added `examples/continuous.c`, a bounded application-controlled loop that
  appends, reads status, checkpoints, marks the durable export boundary, expires
  with an application cutoff, reopens and reads surviving sequence gaps. Its
  control flow handles WAL exhaustion, main-file/retention capacity rejection
  and poisoned handles without adding library policy.
- Registered the example in the shared inventory and Make build with exact
  expected output.
- Extended the existing native batch test with a simple independent batch and
  segment model. The workload repeatedly appends/checkpoints, retains, reopens
  and compares sequential and range records, batch sequences, aggregation,
  logical byte/count status and the historical timestamp floor. It includes
  equal timestamps, a segment spanning a cutoff, pending WAL, complete installed
  expiration and appends observed from `TIMELITE_END`.
- Reviewed the combined operations against the existing failure-injection
  coverage. No database implementation defect was found, so no crash matrix was
  duplicated and no library API or implementation change was needed.
- Clarified the public retention capacity formula, added conservative capacity
  planning and continuous-operation guidance to README, and separated local,
  CI, Windows runtime/cross-build, ARM32, deployment-device and physical
  power-loss evidence. Updated the agent entry-point summaries for Feature 011.

## Verification

Final iteration `make check`: PASS 11, SKIP 1 (the optional Windows-only fault
test), 9.5 seconds. Report:
`build/test-runs/run-20260913-151049-spzo14gt`.

`python3 tools/test.py run local`: PASS 23, SKIP 2 (the optional Windows-only
fault test in native and sanitizer), 34.5 seconds. Native and sanitizer batch
behavior, both public examples, the continuous reference-model workload and
runner self-tests passed on supported local storage. Report:
`build/test-runs/run-20260913-151151-744ti3ob`.

This is local macOS 26.6.2 ARM64, 8-byte-pointer evidence with Apple Clang
21.0.0. It is not ARM32 or deployment-device evidence. Tested source is
unchanged after the pass; only this result record was finalized. Testing stopped
under the project stopping rule.

Container preflight is intentionally deferred until a pull request is requested.

Review pass (second agent, same day): re-read the example, the reference-model
workload and the public-comment/README changes against `timelite.c`. The
retention capacity formula matches the implementation (`bytes >
TIMELITE_DATABASE_CAPACITY - private_data_end` rejects before writes, and
`installed_bytes` is exactly `private_data_end - 160`), the example's expected
output matches its traced behaviour, and the model's expiry rule (whole
installed segments with maximum timestamp below the cutoff) matches the library.
No code defect was found. Two README wordings were tightened: the capacity
formula no longer hedges with "normally", and the Windows runtime evidence
sentence now names what that profile executes. `make check` on the unchanged
source: PASS 11, SKIP 1, 9.3 seconds, report
`build/test-runs/run-20260913-151546-y9wfudzz`. The earlier `run local` report
above was confirmed to cover byte-identical tested source, so it was not rerun
for documentation-only edits.

## Remaining work and limits

- CI, Windows runtime, ARM32, deployment-device and physical power-loss evidence
  remain separate from local validation and will not be inferred from it.
- Container preflight remains deferred until a pull request is requested.
- Release publication, version finalization and maintainer review remain outside
  this feature. The integration workload found no need for another database
  feature before v0.1.

No Git mutations were performed. Work remains uncommitted on the user-selected
`main` checkout for the user to place on a feature branch.
