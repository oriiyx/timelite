# 014: Target-device durability evidence

Status: tooling in progress; evidence objective remains outstanding.
The user requested one agent, the current clean main checkout and no Git mutations.

## Target and execution gates

Device model, ISA, OS/kernel, float ABI, libc, compiler/sysroot, storage device,
filesystem, mount/cache configuration, access, disposable paths and independent
controller/power hardware are UNKNOWN. Requested from the user. No toolchain
is selected and no device compatibility is claimed. Physical cuts require
explicit authorization identifying target, disposable scope and power action.

## Oracle designed before implementation

Use a serialized line protocol over a dedicated SSH stdin/stdout connection.
The independent controller writes and durably flushes an intent before sending
one operation. The device emits a response only after the API succeeds. The
controller durably flushes the response before sending another operation.
A response lost in transport or not durably journaled is uncertain, never an
acknowledged loss. One outstanding operation bounds possible outcomes.

Each append has one record: series 14, timestamp and value equal to its supplied
unique monotonically increasing integer ID. Record returned sequence separately.
Checkpoint partitions acknowledged appends into known immutable segments.
Retention permits removal only of entire installed segments below its cutoff;
pending WAL remains. Unacknowledged retention permits exactly old or new retained
sets, not arbitrary missing eligible records. Successful retention requires new.
After recovery compare every record and sequence with these alternative states;
inspection coherence alone is insufficient. Verify historical timestamp floor,
and use a continued append to verify the next sequence, including after complete
expiration. An unacknowledged append may be present or absent with its predicted
sequence; subsequent IDs must never reuse its ID.

## Planned implementation and verification

Add an opt-in ARM32 compile-only inventory profile requiring an explicit target
configuration, a bounded command workload and controller/oracle tooling, and
an evidence-preserving restart procedure. No library/API/format changes.
Run make check and local; cross compilation only with confirmed toolchain.
Device baseline and physical campaign remain blocked on target/access.

## Campaign totals

Physical trials 0; device runtime trials 0; successes 0; failures 0; ambiguous 0.
No physical cut phase has been exercised. Tooling tests will be recorded separately.

## Completed tooling

Hardware is unavailable; the user explicitly deferred its execution. Target and
SDK details remain unknown. Added an opt-in ARM32 Linux inventory build with
explicit SDK configuration, recorded compiler/triple/macros/configuration,
ARM32/float ABI validation and a compile/link 64-bit off_t assertion. There is
no default ARM toolchain selection and no target binary execution. Added the
standalone inspect executable and bounded workload to the shared inventory.

Added tools/target/workload.c (public API only), evidence.py (durable independent
controller intent/ack journal, strict record/segment oracle, exclusive artifact
preservation and verify-before-status on a copy), plan.txt and the full target
protocol. README links setup, invocation and evidence boundaries. Self-tests
cover missing acknowledgements, old/new retention, illegal partial deletion,
WAL preservation, lost response uncertainty, timestamp/sequence high-water,
missing/mismatched cross configuration, and host workload -> journal -> copied
inspection/recovery -> complete retention -> continued append. No production
code or format was changed; no library defects exposed. A controller pipe
resource warning found during iteration was fixed by explicitly closing pipes.

AGENTS.md and CLAUDE.md were reviewed; their existing device qualification and
separate evidence rules already apply. No shared workflow decision changed.
The user-specific current-checkout/no-Git instruction is recorded here only.

## Verification to date

`make check`: PASS 14, optional SKIP 1, 12.5 s. Report:
`build/test-runs/run-20260913-231324-mq7_hv3m/report.json`.
An intermediate targeted controller/oracle run passed 9 tests; final expanded
self-tests and local verification are pending below. This native run predates
final tooling/documentation updates and is iteration evidence only.

ARM32 compilation: NOT RUN (matching target/SDK unknown). Device runtime:
NOT RUN (hardware unavailable, deferred). Physical cuts: NOT RUN, zero trials.
Simulated evidence consists of oracle fixtures, transport-loss checks and the
existing storage model; no process-kill campaign or physical cut is claimed.
Actual hardware trial counts remain zero. No new target durability claim.

## Final host results and artifact locations

`python3 tools/test.py run local`: PASS 29, optional SKIP 2, 40.7 s;
source unchanged throughout the run. Native and sanitizer storage probe/batch
coverage passed on this macOS host's supported storage. Runner self-tests include
11 Feature 014 checks. Full report, source identity, compiler/platform metadata,
commands, logs and JUnit:
`build/test-runs/run-20260913-231609-ihqchjtc/report.json`
`build/test-runs/run-20260913-231609-ihqchjtc/junit.xml`
`build/test-runs/run-20260913-231609-ihqchjtc/runner/unittest.log`.

The host integration journal acknowledged batches 1..4 (IDs 100..400), expired
complete installed segments, verified the empty retained set with floor 400,
and appended ID 600 with sequence 5 after reopening a working copy. Oracle
fixtures separately rejected loss, partial segment removal and sequence reuse;
allowed whole old/new interrupted-retention outcomes; and preserved pending WAL.
The transport-loss simulation produced no false acknowledgement. Temporary unit
fixtures are removed by test cleanup; durable run logs/snapshots above remain.
These are host tooling tests, not hardware campaign trials.

`python3 tools/test.py run arm32-cross`: expected BLOCKED 15 plus the runner's
aggregate FAIL row (no test ran); no compiler invoked, because confirmed target
configuration is absent. Report:
`build/test-runs/run-20260913-231640-hge06tjj/report.json`.
This checks the gate only and is not ARM32 compilation evidence.

`git diff --check`: passed. No Git mutations. After checks passed, only this
results note was finalized; implementation and tests are unchanged. No repeated
checks needed. CI, Windows runtime and container preflight were not run here.

## Outstanding objective

Confirm the exact target/ISA/OS/kernel/ABI/libc and immutable matching SDK, then
run ARM32 compilation. Obtain hardware/access, identify disposable storage and
its mount/cache configuration and the independent durable controller. Run the
unaltered real-device local baseline on the selected filesystem, and obtain
explicit power-action authorization before physical trials. Execute and retain
all campaign trials, including failed recovery and ambiguous outcomes, and
populate the phase/timing ledger. Device evidence objective remains incomplete.
