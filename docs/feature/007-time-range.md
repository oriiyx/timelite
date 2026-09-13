# 007: Time-range reads

Status: implemented and verified locally and by preflight; committed on the
requested feature branch. No push or pull request.
Branch: feature/007-time-range, from local main at 985ebd75e5a2; initial
tree/index clean.

## Desired state

Globally non-decreasing append timestamps, indexed segment seek, and half-open
window reads with an optional series filter. No allocation, retained buffers,
floating point, backend changes or WAL frame changes. Existing sequential APIs
keep their contracts. Windows durable provisioning remains ENOTSUP.

Out of scope: aggregation, per-series indexes, retention, compaction, automatic
checkpointing, threads, multi-process access, migration tooling, physical
power-cut testing and ARM32 hardware runs.

## Design decisions (recorded before implementation, with compatibility refinement)

All disk integers are little endian. Pair headers, WAL frames, slot offsets and
CRC-32 algorithm remain unchanged. A 48-byte span header alone cannot support
random access to variable-size segments. Use 64 bytes with two backward links;
no growing in-memory index or copied directory is needed.

| Segment offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | TLSPAN07 (ordered), TLUNOR07 (legacy unordered WAL) |
| 8 | 8 | first batch sequence |
| 16 | 8 | last batch sequence |
| 24 | 4 | body length |
| 28 | 8 | minimum timestamp |
| 36 | 8 | maximum timestamp |
| 44 | 8 | previous segment offset, zero for first |
| 52 | 8 | segment n-lowbit(n) offset, zero when n is a power of two |
| 60 | 4 | CRC of bytes 0..59 |

Ordinal n is one-based generation. Backward links allow locating an ordinal by
skipping powers of two or taking the predecessor. Seek binary-searches the
ordered span-bearing suffix by maximum timestamp, then scans candidate frames
and WAL. Span-less TLSEGMNT headers retain their exact 006 32-byte layout and
are scanned linearly as full-range segments. Open validates the whole header
chain and links with a bounded 24-entry local offset table (the capacity permits
fewer than 2^24 segments). No installed payload scan for new layouts.

Manifest offsets 32..39 hold the last installed record timestamp, 40..47 the
last segment offset, 48..51 format marker 7, 52..59 zero, CRC stays at 60.
Generation zero creation remains byte-identical to 006. The marker distinguishes
a real zero timestamp from absent metadata. Feature 006 fails closed on an
installed 007 database because these formerly reserved bytes are nonzero (and
new segment magic is unknown), the deliberate forward-incompatibility pattern
006 used against 004. No in-place header migration.

For old installed data, recover the last timestamp by scanning only the final
legacy segment, not the full database. Existing unordered legacy data stays
readable. WAL recovery obtains the last committed record timestamp during its
existing scan. A legacy unordered WAL gets the same spans and links but
TLUNOR07 magic at checkpoint;
new appends themselves must always be globally ordered. This compatibility
exception prevents an invalid ordered index being asserted for old records.

Append rejects decreasing adjacent times or a first time below the last committed
record time with OUT_OF_ORDER (-9), before any I/O or handle/output effect.
Equal times and all series IDs are allowed. Global order gives exact segment
spans without per-series handle state. A successful append updates last time.

New caller-owned filter: from_us, until_us, series, filter_series (0 or 1).
next_range returns matching records from one batch, skips batches with no
matches, shares the cursor, and returns END when no more matches exist. Invalid
interval (from > until) or flag returns EINVAL; empty interval returns END.
Output capacity is measured against matching records only. Errors and END keep
outputs/cursor unchanged, including after skipped batches; scratch may change.
Seek commits its new cursor only on success or END; END positions at physical
end so later serialized appends remain visible. All operations validate closed,
poisoned and scratch states like next. No query writes or retained filters.

## Write and sync order / interruption table

Checkpoint retains the 006 order: optional legacy slot initialization + sync;
new header and verbatim validated frames at unreferenced data_end + sync;
new manifest in inactive slot + sync; WAL truncate + sync. Header grows from
32 to 64 for every new checkpoint; WAL cursor c translates to D + 32 + c.
Links and spans are synced before their manifest references them.

| Boundary | Recovery |
| --- | --- |
| Legacy slot write/sync before or after effect | WAL remains authoritative |
| Header/frame writes or data sync before or after effect | Old manifest and WAL, orphan ignored |
| Manifest write torn/unsynced | Old or complete new generation, both retain commits |
| Manifest sync after effect | New timestamp and segment installed, stale WAL reclaimed |
| WAL truncate/sync before or after effect | New manifest authoritative, reclaim finished |

## Planned verification

Independent Python struct/zlib golden generator and CRCs are recorded below.
Native order/seek/filter/cursor/retry/006 cases in batch_test.c. Model interruption
boundaries, recovered timestamp checks and golden bytes in batch_model_test.c.
Run make check, python3 tools/test.py run local, python3 tools/test.py prepare
once, then python3 tools/test.py run preflight. Report profile statuses verbatim,
including BLOCKED / NOT RUN. No new inventory entry unless adding a C test.

## Actual changes

- Public OUT_OF_ORDER (-9), caller-owned timelite_range, timelite_batches_seek
  and timelite_batches_next_range. Cursor publication uses a borrowed internal
  view; only cursor fields are copied back. No duplicate ownership or close.
- 64-byte TLSPAN07 headers carry exact spans and backward links. TLUNOR07 uses
  the same layout for old unordered WAL records. Both remain immutable after
  installation. Each new checkpoint records spans, including the legacy case.
- Manifest last timestamp, last segment and format marker are covered by the
  existing CRC and inactive-slot install protocol. Zero timestamp is unambiguous.
- Open validates links using 24 bounded local offsets, recovers installed last
  time from metadata, and obtains WAL times while performing its existing scan.
  Old manifests require a scan of only the final legacy segment. No allocations.
- Binary search over the ordered suffix uses ordinal lookup through backward
  links (O(log^2 S) header reads per lookup, O(log^3 S) worst-case seek headers,
  plus candidate frames/WAL). Legacy prefixes scan linearly. Open is O(S) headers.
- Native tests cover order/equality, seek boundaries and WAL, filter intervals,
  skipped batches, retries, error cursor preservation and the 006 golden fixture.
- Model adds 32 timestamp recovery boundary combinations and 130 header/manifest
  prefixes, plus the existing 144 checkpoint scenarios. A 128-segment case checks
  each seek result and exactly one selected payload read (zero past end), and
  injects an I/O error at every read in seek and a range scan. Open reads zero
  installed payloads in that new-layout fixture. Legacy unordered/zero time and
  independent ordered/unordered golden headers are covered.
- Example now seeks and filters after reopen; output stays unchanged. README,
  AGENTS and CLAUDE reviewed and updated. Inventory/Make/workflows unchanged.

### Independent golden generator

Run from any directory (Python standard library only):

```sh
python3 - <<'PYCODE'
import struct, zlib
t = 1700000000000000
images = {
    'manifest': struct.pack('<8sQQQQQI8x', b'TLINSTAL', 1, 308, 1, t, 160, 7),
    'ordered': struct.pack('<8sQQIQQQQ', b'TLSPAN07', 1, 1, 84, t, t, 0, 0),
    'unordered': struct.pack('<8sQQIQQQQ', b'TLUNOR07', 1, 2, 168, 20, 100, 0, 0),
}
for name, body in images.items():
    crc = zlib.crc32(body)
    print(name, hex(crc), (body + struct.pack('<I', crc)).hex())
PYCODE
```

CRCs: manifest `0x7a196c0a`, ordered `0xde736a65`, unordered `0x4c2cb733`.
Full byte strings are constants in tests/batch_model_test.c; the legacy native
fixture uses 006's manifest CRC 0x24c894f6 and segment CRC 0x1716a4d6.

Additional contracts: query END preserves outputs; seek END changes the cursor
as documented. Scratch may change on read errors. A backward-link read needed
before checkpoint's first effect can fail without poisoning; re-validation after
the effect point poisons. UINT64_MAX is a valid append time but is excluded by
an exclusive upper endpoint of UINT64_MAX. Use next to retrieve that timestamp.

## Verification results

Host: macOS 26.6.2 arm64, Apple clang 21.0.0 targeting
arm64-apple-darwin25.6.0, Python 3.14.7. Docker via OrbStack uses linux/amd64
emulation on this ARM64 host, with GCC 12.2 and MinGW-w64 GCC 12.

Commands actually run:

- `make check`: initial iteration `FAIL 2, PASS 8, SKIP 1` because existing
  tests still required 006 offsets and unordered append fixtures. After updating
  those expectations, three successive iterations with added coverage each
  reported `PASS 10, SKIP 1`. Last make run:
  `build/test-runs/run-20260913-095540-910xl9xs` (8.1 s). The final additional
  unordered golden assertion was then verified by local and preflight below.
- `python3 tools/test.py run local`: `PASS 21, SKIP 2`, 33.5 s;
  `build/test-runs/run-20260913-095737-fp4b8dll`.
- `python3 tools/test.py prepare`: first attempt `FAIL` because the sandbox
  could not access the Docker socket. The approved retry outside the sandbox
  reported `PASS: image timelite-tests:389ffa5e6fa1c9d8`;
  `build/test-runs/prepare-20260913-095616-5510kxn0`. One successful preparation.
- `python3 tools/test.py run preflight`: first sandbox attempt reported
  `BLOCKED 3, PASS 21, SKIP 2` and `FAIL in 33.1s`. Each of linux, linux32 and
  windows-cross was `BLOCKED` with `prepared image timelite-tests:389ffa5e6fa1c9d8
  unavailable; run: python3 tools/test.py prepare`; image-inspect logs show
  Docker socket access denied. Report: `run-20260913-095812-3epegor9`.
- The approved preflight retry outside the sandbox reported `PASS 44, SKIP 12`,
  `PASS in 80.5s`; `build/test-runs/run-20260913-095937-oafbeaj3`.
  Source digest `b9e0bbcefe2c297c08d3913c7d1468d175c4aa9763f3e1544061ca6f9ba87ed6`,
  `stale: false`. Production code, tests, example and build files did not change
  afterwards; only this results/limits note was finalized. Stopped testing.
- `git diff --check`: passed before final staging.

Per-profile results from successful local/preflight runs (status words and
coverage labels retained verbatim):

| Profile | Results | Coverage |
| --- | --- | --- |
| native | PASS 10, SKIP 1 | native batch behavior exercised on supported storage |
| sanitize | PASS 10, SKIP 1 | native batch behavior exercised on supported storage |
| runner | PASS 1 | runner behavior unit tests |
| linux | PASS 7, SKIP 4 | native batch behavior NOT RUN: storage unsupported; public batch consumer compiled; runtime NOT RUN |
| linux32 | PASS 7, SKIP 4 | native batch behavior NOT RUN: storage unsupported; public batch consumer compiled; runtime NOT RUN |
| windows-cross | PASS 9, SKIP 2 | cross-build |

All skips in the successful runs are optional. Native/sanitize ran the required
sparse capacity fixtures. Container Linux x86-64 and i386 model/backend runs are
emulated, not ARM32 tests and not supported-filesystem batch durability evidence.
Windows cross-builds were not executed. Windows native and physical-device
results are unavailable, and CI was not run for this branch.

## Known gaps

- Windows native, branch CI, physical power cuts and physical-device results
  are unavailable. Exact deployment x86/ARM32 CPU, OS and ABI remain unconfirmed.
- Real Linux batch behavior on ext-family/XFS/Btrfs was NOT RUN here; container
  storage is unsupported. Native macOS/model/cross-build evidence stays separate.
- Old manifests without a timestamp need a scan of the final segment (up to
  64 MiB). Open still walks segment headers; no compaction bounds their count.
- Legacy unordered data requires linear reads and has no retroactive ordering
  promise. Seek follows the last-record predicate even for an unordered batch;
  use rewind plus next_range to find every match in internally unordered history.
- The half-open upper endpoint cannot include UINT64_MAX; next can read it.
- Existing 1 GiB main-file limit and external truncation ambiguity remain:
  cutting the pair to its empty layout is indistinguishable from a fresh pair.
- Aggregation, per-series indexes, retention, compaction, automatic checkpointing,
  migration tooling, threads and multi-process access remain out of scope.
