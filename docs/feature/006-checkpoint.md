# 006: Checkpointing committed WAL batches and safe WAL reclamation

Status: implemented; local, sanitizer, emulated container and Windows
cross-build results recorded below. Not committed, pushed or merged. GitHub
Actions, Windows runtime, ARM32, device and power-cut testing have not run.

Baseline and Git: `origin/main` was fetched on 2026-09-12 and is commit
`1adfb0b` (feature 005, the testing suite), which is also local `main`. The
local branch `codex/005-testing-suite` is an ancestor of `origin/main`, so
`codex/006-checkpoint` branches directly from `origin/main` without a stacked
base. The working tree and index were clean at the start.

This is the checkpointing work that features 003 and 004 deferred (their
historical "feature 005" wording means this feature; note 004 already carries
a scheduling annotation).

## Desired state

A caller can call `timelite_batches_checkpoint`. Every batch committed to the
WAL is durably installed into the main database file as an immutable segment,
install metadata is durably switched to reference it, and only then is the WAL
space reclaimed, so appends can continue past the 64 MiB WAL limit. Reads keep
returning every committed batch exactly once in sequence order, combining
installed segments and the WAL. No interruption at any write or sync boundary
may lose or duplicate a committed batch or invalidate the only committed copy.
Windows keeps returning `ENOTSUP` for durable provisioning; checkpoint gains no
weaker Windows fallback. The v1 API and format, the WAL frame format, and the
32-byte v2 pair headers are unchanged.

Out of scope and deferred: queries, indexes, retention, compaction of installed
segments, migration tooling, threads, multi-process access, automatic
checkpointing, WAL ring management.

## Design decisions

### Main storage layout (database file, version 2 header unchanged)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 32 | Pair header from feature 004 (`TIMELITE`, version 2, identity, CRC) |
| 32 | 64 | Install manifest slot 0 |
| 96 | 64 | Install manifest slot 1 |
| 160 | ... | Installed segments, each a segment header followed by WAL frames |
| data_end | ... | Orphan bytes from an interrupted install; ignored and overwritten |

Creation writes the whole 160-byte prefix in one write: header, a generation 0
manifest in slot 0, and 64 zero bytes in slot 1. A 32-byte database created by
feature 004 ("legacy layout") stays valid: it is read as generation 0 with no
installed data and no slots. Its first checkpoint writes bytes 32..160 (the
generation 0 manifest and zero slot) and syncs before installing anything.

The feature 004 library, given a database of this feature, fails closed with
`TIMELITE_INVALID_DATABASE` before any write (it requires size exactly 32), and
a reclaimed WAL whose first frame is not sequence 1 fails the same way. The
version number stays 2 because the older library already refuses new files
without touching them, and bumping the header version would require an in-place
header rewrite on the legacy upgrade path, which is not atomic. This is a
deliberate forward-incompatibility, not a migration.

### Install manifest (64 bytes, one per slot)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `TLINSTAL` |
| 8 | 8 | Generation: number of installed segments (0 after creation) |
| 16 | 8 | data_end: database offset one past the last installed segment (160 when empty) |
| 24 | 8 | Last installed batch sequence (0 when empty) |
| 32 | 28 | Reserved, must be zero |
| 60 | 4 | CRC-32 of bytes 0..59 |

Generation g always lives in slot g mod 2. Validity: magic, zero reserved
bytes, CRC, 160 <= data_end <= `TIMELITE_DATABASE_CAPACITY`, data_end <= file
size, generation 0 exactly when data_end is 160 and last sequence is 0, and
otherwise last sequence >= generation and data_end >= 160 + 116 * generation
(each segment holds at least one 84-byte frame plus a 32-byte header).

### Segment header (32 bytes, immediately before its frames)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `TLSEGMNT` |
| 8 | 8 | First batch sequence in the segment |
| 16 | 8 | Last batch sequence in the segment |
| 24 | 4 | Body length: bytes of frames that follow (84 * n .. 1344 * n, n = last - first + 1) |
| 28 | 4 | CRC-32 of bytes 0..27 |

The body is the WAL bytes from offset 32 to the committed end, copied verbatim
after re-validating each frame. Frames keep their feature 004 encoding,
sequence numbers and checksums, so installed data is self-validating and the
reader uses the same frame parser for both files.

Golden bytes, generated independently with Python `struct` and `zlib.crc32`:
generation 0 manifest CRC `0x73b64024`; generation 1 manifest (data_end 276,
last sequence 1) CRC `0x24c894f6`; segment header (first 1, last 1, body 84)
CRC `0x1716a4d6`. The model test holds the exact byte strings.

### Checkpoint protocol: exact write and sync order

`timelite_batches_checkpoint(db, scratch, scratch_size)`, with the committed
WAL holding frames S..L at offsets 32..end and the current manifest generation
g (data_end D, last installed S-1):

0. Argument checks (no effect): NULL handle or scratch `EINVAL`; closed handle
   `EBADF`; poisoned handle `TIMELITE_RECOVERY_REQUIRED`; scratch below
   `TIMELITE_BATCH_SCRATCH` `TIMELITE_BUFFER_TOO_SMALL`. Empty WAL: return 0
   with no I/O. If D + 32 + (end - 32) would exceed
   `TIMELITE_DATABASE_CAPACITY`: `TIMELITE_DATABASE_FULL`, no effect, handle
   usable. The handle is then marked failed before the first effect, exactly as
   append does, so any later error leaves `TIMELITE_RECOVERY_REQUIRED`.
1. Legacy layout only: write bytes 32..160 (generation 0 manifest, zero slot),
   sync the database. Safe: nothing references these bytes; if lost or torn,
   recovery treats a database of size <= 160 without a valid manifest as empty.
2. Write the segment header at D and each re-validated frame at
   D + 32 + (offset - 32). Sync the database. Safe: no manifest references
   bytes at or beyond D, so they are orphan bytes until step 3 completes, and
   the WAL is untouched and remains the committed copy. The sync guarantees the
   segment bytes are durable before any metadata points at them.
3. Write the generation g+1 manifest (data_end D', last L) into slot
   (g+1) mod 2, the slot that does not hold generation g. Sync the database.
   This is the install point. Safe: the current slot is never overwritten, so
   a torn or lost write leaves generation g intact and the new segment orphan;
   the WAL still holds every frame. In-place overwrites are never assumed
   atomic; the two-slot generation scheme is what makes the switch atomic.
4. Truncate the WAL to 32 bytes, sync the WAL. This is the reclaim. Safe: the
   manifest is durable, so the WAL frames are redundant; an interrupted or
   failed truncate leaves a stale WAL that recovery recognises and finishes
   truncating (below).
5. Update the handle: installed end, sequence, generation, empty WAL, clear
   failed. The read cursor is preserved: a cursor at WAL offset c moves to
   database offset D + c, because the segment body is a verbatim copy.

Success promises: every batch committed before the call is installed in the
main file and durable under the feature 004 storage contract, the WAL is
empty, the next sequence is unchanged, and reads continue at the same logical
position. Checkpoint is manual only: an automatic checkpoint inside append
would add three syncs and a different failure mode (`TIMELITE_DATABASE_FULL`)
to an operation whose latency the caller controls, and the caller can trigger
it on `TIMELITE_WAL_FULL` instead.

Error results after the effect point poison the handle like append: OS errors
such as `ENOSPC`, `EFBIG`, `EIO`, or `TIMELITE_INVALID_DATABASE` from frame
re-validation, all require close and reopen. The WAL is reclaimed only after
the manifest sync, so a failed checkpoint never removes committed data.

### Recovery and open

After the feature 004 header and identity checks (the implemented rule 4 below
replaced the planned "orphan header" check with a simpler, stronger one):

1. Database size exactly 32: legacy layout, generation 0, no installed data.
2. Otherwise read both slots (a short read is an invalid slot). Choose the
   valid manifest with the highest generation; a valid manifest in the wrong
   slot for its generation, or two valid manifests with equal generations,
   fail closed. No valid manifest: size <= 160 means empty (a manifest with
   generation >= 1 implies a synced segment and size > 160), size > 160 fails
   closed with `TIMELITE_INVALID_DATABASE`.
3. Walk the segment chain from 160: exactly `generation` headers, each valid,
   first sequence continuing from the previous last, bodies inside data_end,
   ending exactly at data_end with the manifest's last sequence L. Frame
   contents are validated when read, so open costs one 32-byte read per
   segment plus the WAL scan, not a scan of the whole main file.
4. Orphan check (after the WAL scan): if the file is larger than data_end,
   the WAL must hold at least one uninstalled frame (it starts at L+1 and is
   not stale). Bytes beyond data_end come only from an interrupted install,
   and that install's frames stay in the WAL until a later manifest is synced,
   after which the file ends exactly at the new data_end. Orphan bytes with an
   empty or stale WAL are therefore the signature of a damaged newest manifest
   after reclaim, or of an externally cut file, and fail closed instead of
   silently hiding installed batches. Legitimate orphan bytes are ignored and
   overwritten by the next checkpoint.
5. WAL scan. Read the first frame header. If its sequence F is L+1, scan as
   in feature 004 with expected sequences from L+1 and truncate a validated
   incomplete suffix. If 1 <= F <= L, the WAL is stale (reclaim interrupted):
   every frame must be complete and valid, sequences must run F..L exactly,
   and after provisioning the WAL is truncated to 32 and synced to finish the
   reclaim. Any other F, an incomplete tail in a stale WAL, or a frame beyond
   L in a stale WAL fails closed.
6. Provision both files, then perform the single truncate and sync decided
   above. Truncation never removes a committed frame that is not installed.

### Reads

`timelite_batches_next` walks installed segments first: at a segment boundary
it reads and validates the header, then reads frames with the segment end as
the bound (a frame that does not fit is corruption, never an incomplete tail).
At data_end it switches to the WAL from offset 32. Cursor, output and count
change only after a whole frame is validated and copied, so
`TIMELITE_BUFFER_TOO_SMALL` still leaves the position unchanged. Memory is the
caller's scratch; there is no index.

### Limits

`TIMELITE_DATABASE_CAPACITY` is 1 GiB (2^30 bytes) of main file including
headers. Checkpoint refuses to grow data_end past it with
`TIMELITE_DATABASE_FULL` before any effect; appends continue until
`TIMELITE_WAL_FULL`, after which the database is effectively read-only until
a later retention or compaction feature. Open validates data_end against the
capacity, which bounds the segment walk to (2^30 - 160) / 116 headers in the
pathological one-batch-per-checkpoint case; checkpointing many batches at a
time keeps open fast. OS `EFBIG`/`ENOSPC` during the data copy poison the
handle; the orphan bytes are ignored on reopen and overwritten later.

### Windows

Durable provisioning still returns `ENOTSUP`, so no batch pair can be open on
Windows and checkpoint is unreachable there; on the closed handle it returns
`EBADF`. No compile-time branch pretends otherwise, and the model test (fake
backend) keeps full checkpoint coverage on `windows-native`. The native batch
test asserts the open `ENOTSUP` result and then the `EBADF` checkpoint result
with no file effects.

## Interruption table and test mapping

Every row is exercised by `tests/batch_model_test.c` (`checkpoint_boundaries`
and `checkpoint_format`) unless another test is named. "Before" means the
operation fails without effect; "after" means the effect happened and the
error or crash follows.

| Interruption point | Recovery outcome | Test |
| --- | --- | --- |
| Legacy upgrade write before/after, upgrade sync before/after | Size <= 160 with no valid manifest is empty; WAL authoritative; upgrade repeats | model `checkpoint_boundaries` legacy loop |
| Segment header or any frame write before/after, data sync before/after | Orphan bytes ignored; WAL authoritative; next checkpoint overwrites | model boundaries 1..k |
| Manifest write before/after (torn or complete but unsynced), manifest sync before | Old manifest chosen; orphan segment with WAL still holding L+1.. passes the orphan check | model boundaries |
| Manifest sync after (install durable, return lost) | New manifest chosen; stale WAL F <= L is verified then truncated | model boundaries |
| WAL truncate before/after, WAL sync before/after | Stale WAL finished or already empty; no duplicate reads | model boundaries |
| Second checkpoint fails at any boundary | First segment remains valid and readable | model `checkpoint_boundaries` second loop |
| Newest manifest damaged after reclaim | Older manifest chosen, file larger than its data_end, WAL empty or starting past L+1: fail closed | model `checkpoint_format` (empty WAL, and WAL with later appends) |
| Main file cut below data_end, WAL empty | Fail closed for every size above 160; a cut to 160 or below equals a fresh database (outside the model, documented) | model and native `database cut` loops |
| Older manifest damaged | Newest chosen; open succeeds | model `checkpoint_format` |
| Manifest in wrong slot, equal generations, bad reserved bytes, data_end beyond size | Fail closed | model `checkpoint_format` |
| Segment header damaged (magic, sequences, length, CRC) | Fail closed at open | model `checkpoint_format`; native `batch_test` bit flips |
| Installed frame damaged | Open succeeds; `next` fails closed at that frame, cursor unchanged | native `batch_test` bit flips; model |
| Stale WAL with damaged frame or frame beyond L | Fail closed, no truncation | model `checkpoint_format` |
| Reclaimed WAL followed by normal appends and crash | Feature 004 recovery from L+1 | model boundaries; native reopen |
| Database at capacity | `TIMELITE_DATABASE_FULL`, no effect, handle usable, appends continue | native `batch_test` sparse fixture (exit 78 if the storage refuses a sparse 1 GiB file) |
| Torn creation image (header complete, slots partial) | Empty database; the empty WAL created just before fails validation, nothing is repaired | model `format_cases` prefixes 0..159 and `checkpoint_format` torn image |
| Read cursor across checkpoint | WAL offset c becomes database offset D + c; END stays END | model `checkpoint_basic` |
| Full 64 MiB WAL checkpointed, appends continue, second checkpoint | All 49,932 + 1 batches read once, in order, after crash and reopen | model `capacity` |
| Windows | Open `ENOTSUP`; checkpoint on closed handle `EBADF` | native `batch_test` Windows branch; model provisioning `ENOTSUP` case |

## Planned verification

`make check` while iterating; `python3 tools/test.py run local` before handing
back; `python3 tools/test.py run preflight` after `prepare`. Container profiles
on this host record native batch coverage as NOT RUN (virtiofs mount), which is
expected. No physical power-cut or device testing is planned or claimed.

## Actual changes

- `timelite.h`: `TIMELITE_DATABASE_CAPACITY`, `TIMELITE_DATABASE_FULL` (-8),
  `timelite_batches_checkpoint`, six new private handle fields (installed end,
  installed sequence, generation, segment end, WAL/segment cursor flag, legacy
  flag) and updated contracts for append and next. No v1 change.
- `timelite.c`: creation writes the 160-byte image; open reads the manifest
  slots, walks the segment chain, detects stale WALs and orphan bytes, and
  finishes an interrupted reclaim; `next` walks segments then the WAL with
  copy-on-success cursor state; `checkpoint` implements steps 0..5 above.
  The frame header check was factored out of the frame parser so the WAL scan
  can classify the first frame. No allocation, no globals, no new file-layer
  operation: write, sync and truncate suffice, so `file_io.h` and both
  backends are unchanged.
- `tests/batch_model_test.c`: database model grown to 64 MiB + 64 KiB
  (static, test only); zero-fill of write gaps like both backends; golden
  bytes for the creation image, generation 1 manifest and segment header;
  `checkpoint_basic`, `checkpoint_boundaries` (7 regular and 10 legacy
  write/sync boundaries plus truncate before/after, each before/after effect
  and with/without unsynced bytes surviving: 144 crash scenarios) and
  `checkpoint_format` (manifest, reserved, data_end, wrong slot, segment
  header and every installed byte flip, stale WAL finished/damaged/extended/
  incomplete, cut database, torn creation image, `ENOTSUP` provisioning);
  `capacity` now checkpoints the full WAL and appends beyond it.
- `tests/batch_test.c`: checkpoint golden bytes on real storage, installed
  then WAL reads, append after reclaim, five append/checkpoint cycles read
  twice across reopen, every installed byte flip, cut database sizes, the
  sparse 1 GiB capacity fixture (exit 78 when refused) and the Windows/
  unsupported `EBADF` assertion after `ENOTSUP`.
- `examples/batches.c` and the inventory: the public example checkpoints
  between two appends and prints both batches after reopen.
- README, AGENTS.md, CLAUDE.md, notes 003 and 004 updated as described above.

## Verification results

Host: macOS 26.6.2 on Apple Silicon (arm64), Apple clang 21.0.0, Python
3.14.7, Docker via OrbStack (containers run linux/amd64 under emulation and
the report records `emulated: true`). The Docker image from feature 005 was
already present, so `prepare` was not rerun. Docker is unreachable inside the
command sandbox used for most of this session; the preflight run was executed
outside it, which changes nothing about the commands.

Commands actually run, in order, on the finished tree except for this results
section and the status line, which were written afterwards:

- `make check` during iteration: first run FAIL only in `batch_model` (its
  64-byte database model could not hold the new layout), then repeated
  `python3 tools/test.py run native --test batch_model` runs that caught four
  test-side mistakes in turn (a wrong expectation for torn creation prefixes,
  a missing handle re-initialisation after a modelled crash, a legacy loop
  with two instead of three frames, and truncate counters compared against
  zero instead of a snapshot); no production change came out of these.
  `python3 tools/test.py run native --test batch` once failed to build (an
  `assert` inside a conditional expression) and passed after the fix.
- `make check`: PASS 10, SKIP 1 (optional Windows-only fault test).
- `python3 tools/test.py run local --jobs 4`: PASS 21, SKIP 2 (optional),
  33.0 s. `native` and `sanitize` each PASS 10 with `batch` recorded as
  "native batch behavior exercised on supported storage" (APFS); the capacity
  fixture ran (no exit 78); `runner` PASS with 15 unit tests.
- `python3 tools/test.py run preflight --jobs 4 --timeout 300`: PASS 44,
  SKIP 12, all skips optional, not stale, 66.3 s (source digest
  `3c2fe29a03a4cd39…`, commit `1adfb0b` dirty). Per profile: `native` and
  `sanitize` PASS 10 each as above; `runner` PASS; `linux` (target
  linux/x86_64, emulated) and `linux32` (target linux/i386, emulated) PASS 7
  each with `storage_probe`, `batch` and `batches` recorded as optional skips
  "native batch behavior NOT RUN: storage unsupported" (virtiofs mount, as
  expected on this host); `windows-cross` PASS 9 cross-builds including
  `batch_model` and `batch` with the Windows backend, nothing executed.
- `git diff --check`: passed.

Model runtime: 4.4 s native, 8.1 s under sanitizers, 17.0 s in the emulated
32-bit container; the static model buffers are now about 260 MiB.

Not run and not claimed: GitHub Actions for this branch, `windows-native`
(no Windows host here), real Linux ext4/XFS/Btrfs runtime (containers only
saw an unsupported mount), ARM32 or device builds, and physical power-cut
testing. Model interruptions are deterministic simulations under the stated
storage assumptions, not hardware evidence.

## Known gaps and remaining work

- The native fill past 64 MiB is the bounded five-cycle variant: each native
  append costs two device flushes, so the exhaustive fill runs only in the
  model. Real Linux ext4/XFS/Btrfs runtime is exercised only by CI.
- A database cut externally to exactly 160 bytes or less looks like a fresh
  database (as external WAL truncation did in feature 004). Detecting it would
  need the WAL header to carry the installed sequence, which would change the
  32-byte WAL header; deferred.
- Installed frame contents are validated at read time, not at open; open
  validates manifests and every segment header.
- Open cost grows with the segment count (one 32-byte read each); there is no
  compaction or retention, and the 1 GiB main file is a hard stop.
- Physical power-cut, device firmware and target hardware (x86, ARM32)
  behaviour remain unproven; the model and native tests are not that evidence.
- Queries, indexes, retention, compaction, migration tooling, automatic
  checkpointing, threads and multi-process access remain deferred.
