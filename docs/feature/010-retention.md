# 010: Explicit whole-segment retention

Status: implemented and verified locally; ready for user review. The user
explicitly selected the current main checkout for this work and retains all Git
mutations. Initial working tree was clean.

## Contract and scope

`timelite_batches_expire_before(db, cutoff_us, scratch, scratch_size)` removes
only installed segments whose every timestamp is strictly below the cutoff.
Pending WAL is untouched. No automatic checkpoint or policy. Retained frames
keep their bytes and sequences. Sequence high-water and append timestamp floor
survive complete expiration. Counts describe currently retained batches;
last_timestamp_us remains the historical append floor even when counts are zero.
The cursor keeps its next surviving sequence, skipping expired batches; END
stays at its sequence position and can observe subsequent appends.

## Storage protocol

Keep the pair headers, WAL and segment encodings. Add a retention manifest
encoding in the existing two 64-byte slots, separating metadata revision from
segment count and adding a root offset and recovery phase. Revision parity
selects the slot. Persist installed sequence high-water and timestamp floor
independently of the surviving segments. Sequence gaps between segments become
valid only with this explicit new encoding. Opening old data does not migrate
it; an effective retention call explicitly installs the new encoding. Older
libraries reject the completed new layout. No automatic format migration.

1. Scan headers (legacy segments require validated frame scans) and calculate
   retained bytes. No eligible segment is a successful no-op. Reject before
   writes if original end plus retained bytes exceeds the unchanged 1 GiB cap.
2. Publish and sync a PREPARE manifest referencing the original data. This
   authorizes recovery to discard an interrupted temporary tail, even with an
   empty WAL. Before this sync there are no tail writes.
3. Copy retained segments to the original logical end, rebuild physical links,
   validate copied frames and sync. Publish and sync a TAIL manifest referencing
   only this durable copy. Expiration takes effect at this switch.
4. Copy that authoritative set to offset 160 and sync, then publish and sync a
   CLEANUP manifest referencing the front copy. The destination cannot overlap
   the tail: retained bytes are no greater than original segment bytes.
5. Truncate to the front copy end, sync, then publish and sync NORMAL metadata.
   Space is now reusable by ordinary checkpoints. Complete expiration skips
   data copying and needs no temporary payload space.

Recovery after provisioning discards a PREPARE tail (old state), repeats a TAIL
copy, or finishes CLEANUP truncation (new state). At every copy and metadata
boundary the authoritative data remains untouched. A failed write/sync/truncate
poisons the handle; close and reopen are required. Pre-effect validation/read
errors preserve the handle. Recovery failures close acquired resources once.
Existing qualified filesystem/flush assumptions remain; no physical power-loss
certification. Arbitrary external modification remains outside the contract.

## Capacity and limits

Retention may return TIMELITE_DATABASE_FULL near capacity when it cannot fit a
temporary retained copy. Applications must call early enough or select a cutoff
that expires more segments; no capacity increase or extra file is introduced.
Whole segments spanning the cutoff stay intact, including their older records.
Copying is internal to retention, with bounded locals and caller scratch.

## Verification plan

Extend native and model behavior tests and focus interruption injection on the
new writes, syncs, truncation and recovery. Run make check during iteration and
python3 tools/test.py run local once ready. Container preflight is deferred.
Iteration checks are recorded below.

## Exact manifest encoding and compatibility

All integer fields are little endian. Each existing 64-byte slot now supports:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `TLRETAIN` |
| 8 | 8 | Monotonic metadata revision; selects slot by parity |
| 16 | 8 | Authoritative data end |
| 24 | 8 | Installed sequence high-water, including expired batches |
| 32 | 8 | Historical last installed timestamp, including expired batches |
| 40 | 4 | Last surviving segment offset, zero if none |
| 44 | 4 | First segment/root offset; 160 except during TAIL |
| 48 | 4 | Format marker 10 |
| 52 | 4 | Surviving segment count |
| 56 | 4 | Phase: 0 NORMAL, 1 PREPARE, 2 TAIL, 3 CLEANUP |
| 60 | 4 | CRC-32 of bytes 0..59 |

The 1 GiB capacity makes 32-bit physical offsets and segment counts sufficient.
Revision is no longer a segment ordinal. Header walks validate counts, extents,
links and strictly increasing segment sequence spans, allowing deleted gaps.
Live batch count is the sum of surviving spans; pending count is committed
sequence high-water minus installed high-water. The WAL still starts at the
next installed sequence. Checkpoint keeps this format once retention enables it.
Revision exhaustion returns EOVERFLOW; retention reserves enough revisions to
finish recovery before any effect. No-op calls need no revision.

An empty retained file is truncated to 161 bytes with logical end 160. The
unreferenced extra byte prevents older readers' legacy-empty fallback (size at
most 160) from accepting a database whose historical sequence floor is nonzero.
Both slots have new magic by the deletion switch. Before that switch, an older
valid slot still describes the coherent original data. Pair headers never change.
Successful retention does not change 32-byte legacy segment headers or frames;
64-byte segment links and their CRCs are rebuilt at each physical destination.
The qualified storage assumptions include preserving synced bytes during later
writes to the same physical sector; no atomic in-place overwrite is assumed.

## Persistence boundaries and recovery results

| Failure/interruption | Authoritative recovery state |
| --- | --- |
| PREPARE write/sync | Original segments; no tail written until sync succeeds |
| Tail header/frame write or tail sync | Original segments; PREPARE discards orphan tail |
| TAIL manifest write/sync | Original or durable retained tail; both preserve retained frames |
| Front header/frame write or front sync | Durable retained tail; repeat front copy |
| CLEANUP manifest write/sync | Durable tail or durable front copy |
| Truncate or following sync | Durable front copy; repeat shrink |
| NORMAL write/sync | Durable front copy; CLEANUP is safe to repeat |
| Recovery copy/manifest/truncate/sync | Same phase protocol; failed open closes both resources once |

Validation and capacity rejection before the first write preserve the entire
handle. Later read/revalidation errors also poison it, even if deletion already
committed. A caller must close/reopen and inspect recovered data before deciding
whether to retry. Scratch is unspecified after calls. No retained buffers or
ownership transfers are introduced. Repositioning the cursor can scan surviving
frames; relocation copies retained bytes twice. Span-less legacy eligibility
scans all records. The operation has bounded memory, not bounded latency.

Legacy unordered history can leave retained timestamps above the historical
append floor when a later segment expires. Range early stopping and subsequent
checkpoint ordering compare against the surviving tail span, separately from
that historical floor. Queries keep their existing legacy semantics.

## Actual changes and verification

Implemented the API, recovery-phase manifest encoding, internal copies and
truncation in timelite.c/h. Existing backends, WAL/frame formats, inventory and
build workflows are unchanged. README example and cursor/status/capacity/error
contracts updated. AGENTS.md and CLAUDE.md project summaries now describe
retention; their shared workflow rules remain unchanged.

Native tests cover strict/spanning cutoffs, retained reads/ranges/aggregation,
status, reopen, whole legacy deletion, sequence and timestamp preservation, and
physical file shrink/reuse. The sparse 1 GiB fixture now has span-bearing headers
and checks pre-effect temporary-space rejection, complete expiration, and real
subsequent append/checkpoints. Its synthetic bodies are not read and do not
constitute record-validation or durability evidence.

Model tests cover all cursor positions and END/future appends, zero/equal/MAX
cutoffs, non-prefix expiration, mixed 006/007 segments, pending and empty WALs,
retained queries, status/high-water marks, repeat/reopen, arguments and handle
states. They inject every write/sync and truncate before/after effect for partial
and complete expiration, with unsynced bytes either surviving or disappearing;
every successful-path retention read; every write/sync/truncate in all three
recovery phases; every partial prefix of representative new manifest, header
and frame writes; and checkpoint persistence boundaries after retention.
Ownership and coherent retained content are checked after recovery. Existing
unrelated append/checkpoint crash matrices were not duplicated.

`make check` passed during successive coverage additions. One iteration failed
only because the added legacy model case appended after its fixture had closed
the handle; the test placement was corrected. Final `make check`: PASS 10, SKIP 1 (optional Windows-only fault test),
8.3 seconds; report `build/test-runs/run-20260913-145745-8xz47j3x`.
`git diff --check` passed.

`python3 tools/test.py run local`: PASS 21, SKIP 2 (optional Windows-only
fault test in native and sanitize), 33.8 seconds. Native and sanitizer batch
behavior ran on supported storage; model and runner self-tests passed.
Report: `build/test-runs/run-20260913-145853-60ttq14v`.
Implementation and test source are unchanged after the pass; only this result
record was finalized. Stopped testing. No Git mutations were performed.
Container preflight, CI, Windows runtime, ARM32/device and physical power-loss
checks are not run for this feature. Existing Windows ENOTSUP remains unchanged.
