# 004: Durable batch append and recovery

Status: implemented and verified as recorded below; checkpointing deferred.

Scheduling note (2026-09-12): this note originally assigned checkpointing to
feature 005. Feature 005 became the repeatable testing suite
([005-testing-suite.md](005-testing-suite.md)). Checkpointing, main storage
and WAL reclamation remain deferred with no number reserved. Mentions of
"feature 005" below are historical and mean that later checkpointing feature.
Prerequisite verified: freshly fetched origin/main e307196 includes feature 003.
Branch: codex/004-durable-batch. Initial worktree and index were clean.

## Original design decisions before coding

Keep the v1 lifecycle API unchanged. Add a separate caller-owned batch handle and
v2 pair API; v1 migration is deferred and batch open rejects v1 without writes.
Use explicit database and WAL paths, 128-bit OS-random identity, and checksummed
versioned headers. Never repair partial pair creation or create a missing member
of an existing pair. Exclusive creation can leave owned partial files; never
unlink on failure. Existing mismatched pairs are rejected before sync or repair.

Records contain uint32 series (zero allowed), uint64 microseconds since Unix epoch,
and int64 fixed-point value in application-selected units. No floating-point
assumptions, tags, schema, or per-record types. All bit patterns are valid; order
and duplicate timestamps are preserved. Empty batches are rejected. Maximum 64
records per batch. Scratch is caller-owned; no pointer retained. Encoded signed
values use explicitly defined two's complement arithmetic, independent of native
signed representation. Sequence starts at 1 and counts committed batches.

WAL limit is fixed at 64 MiB, including its header. No reclamation until 005.
Each batch has checksummed framing, bounded record bytes and a final commit that
binds sequence, count and body checksum. Write and sync body before writing commit,
then sync commit. Partial or malformed framing fails closed. A validated header
whose declared end exceeds EOF is an incomplete suffix; a fully present batch
must pass all integrity checks. A checksum error never licenses truncation.
Recovery validates all complete batches before truncating a provably incomplete
suffix and syncing. Interrupted truncation is retried on reopen. Reader copies
output only after validating a whole batch in scratch; small buffers leave cursor
and outputs unchanged. Readers use a handle-local cursor and see later serialized
successful appends at end-of-data. Failed append poisons reads and writes until
close/reopen to avoid presenting an uncertain commit as acknowledged.

## Original provisioning questions (resolved below)

Linux requires file sync plus parent-directory fsync. macOS requires directory
fsync followed by F_FULLFSYNC to drain device caches. Directory ancestry must
already be durable and must remain stable throughout ownership. Verify the final
path identifies the opened inode, rather than infer that from a filename. Reject
symlink final components for durable provisioning. Unsupported flush operations
must fail, never fall back. Windows file FlushFileBuffers alone does not establish
namespace durability; a supported provisioning mechanism must be established or
the durability-dependent API must return an explicit unsupported error.
Storage must honor flushes, ordering and preservation of earlier synced bytes;
there is no universal guarantee for dishonest hardware or arbitrary corruption.

Official sources reviewed:
- https://man7.org/linux/man-pages/man2/fsync.2.html
- https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers

## Operation failure contract

| Boundary | Effects and ownership |
| --- | --- |
| Argument/count/scratch/capacity rejection | No I/O, unchanged handle and outputs |
| Identity source fails | No files created; closed reusable handle |
| Exclusive pair creation fails | May leave partial new files; close acquired resources once; never delete |
| Existing pair read/validation fails | No writes; close both once, primary error wins |
| Provisioning sync or path association fails | Closed handle; names/data may have persisted; no deletion |
| Recovery truncate/sync fails | Closed handle; suffix may remain or be removed; prior commits retained |
| Append body write/sync fails | Uncertain file effects, poisoned handle; close/reopen required |
| Commit write/sync fails | Batch may be committed after restart; poisoned handle |
| Final sync succeeds, return lost | Committed batch survives under stated storage assumptions |
| Read fails or capacity insufficient | No validated output or cursor publication |
| Close fails | Consumed handle, uncertain native release, no retry; first error wins |

No exactly-once retries: reopen and enumerate sequences and contents. Identical
application batches cannot establish whether a retry or an independent operation
was intended. Callers must retain their own reconciliation context.

## Original verification plan

Implement shared backend tests, deterministic volatile/persisted storage model,
truncation prefixes, integrity damage, capacity, buffer and state semantics,
creation races and partial pairs, short transfers and injected operation failures.
Preserve all existing tests and Windows smoke builds; extend Make and CI. Run local
strict and sanitizer checks, available Linux and Windows cross builds. Report
Windows runtime, CI, 32-bit/device and physical power-cut results independently.
Exact native provisioning support and detailed binary layout remain to be finalized
before implementing the corresponding operation. Checkpointing, segments, WAL
reset/reuse, rotation, indexes and other query APIs are excluded.

## Implemented API and format (final design)

The earlier design section records the pre-implementation plan. The implementation
is now in timelite.c/h; native operations remain in file_io.c/file_io_windows.c.
There are no production allocations or mutable globals. A separate
`struct timelite_batches` avoids changing v1 handle ownership or on-disk behavior.
`timelite_batches_init/open/append/next/rewind/close` implement the v2 API.
An open handle owns both native resources. All fields are private. Scratch and
record buffers may be reused as soon as a call returns and must not alias one
another, outputs, or handles. The core uses only small fixed local header buffers;
provisioning uses a 4096-byte parent-path buffer on the stack. POSIX parent paths
that do not fit are rejected; ordinary backend path limits also apply.

Record fields: uint32 series, uint64 timestamp_us since Unix epoch (0 through
UINT64_MAX, approximately 584,554 years), int64 value. Application-selected units
are a convention outside the format. Zero series, out-of-order readings, duplicate
timestamps, INT64_MIN/MAX and every other integer value are accepted. Empty batches
and counts above 64 return EINVAL. No schema, tags, type codes or floating point.

All integers on disk are little endian, including signed values encoded modulo
2^64. Decoding avoids an out-of-range unsigned-to-signed cast. No structs are
written to disk. Frame sizes are computed only after checking count <=64.
File sizes are checked before scanning; each scan makes positive progress and is
bounded by 64 MiB. No in-memory index grows with WAL size. Worst-case full scan
is linear in capacity (up to 798,914 minimum-size batches). Scratch must be at
least 1344 bytes for open, append and next regardless of current count. Encoded
batch size is 64 + 20N, maximum 1344. Output storage for 64 public record structs
is sufficient; its native size/padding is not the encoded size.

### File headers (32 bytes each)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | DB `TIMELITE`; WAL `TIMEWAL!` |
| 8 | 4 | Version 2 |
| 12 | 16 | Same random database identity in both files |
| 28 | 4 | CRC-32 of bytes 0..27 |

DB must remain exactly 32 bytes. WAL starts with its header, followed by batches;
there are no reserved capacity bytes, segments, install metadata or generations.
The identity covers the single current pair. WAL rotation/reuse is excluded.
Unknown nonzero versions return UNSUPPORTED_VERSION before interpreting their
body; zero version or invalid magic/length/integrity returns INVALID_DATABASE.
No v1 migration: batch open rejects v1, legacy open rejects v2, neither modifies it.

### Batch header (32 bytes)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `TLBATCH!` |
| 8 | 8 | Sequence, exactly preceding committed sequence + 1 |
| 16 | 4 | Record count N, 1..64 |
| 20 | 4 | Body length, exactly 32 + 20N |
| 24 | 4 | Reserved, must be zero |
| 28 | 4 | CRC-32 of header bytes 0..27 |

The next N records each contain series at offset 0 (4 bytes), timestamp_us at
4 (8 bytes) and signed value at 12 (8 bytes). Records retain caller order.
The body is the header plus these records.

### Final commit (32 bytes)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `TLCOMMIT` |
| 8 | 8 | Same sequence as body |
| 16 | 4 | Same record count |
| 20 | 4 | Same body length |
| 24 | 4 | CRC-32 of complete body including its header CRC |
| 28 | 4 | CRC-32 of commit bytes 0..27 |

CRC-32/ISO-HDLC uses reflected polynomial 0xedb88320, initial/final XOR
0xffffffff. Golden fixture independently generated using Python struct and
zlib.crc32: series 17, time 1700000000000000, value -123, sequence 1 gives body
CRC 0x22c6780c and commit CRC 0x72daf6bb. Integrity is accidental-error detection
with finite collision probability, never authentication or a cryptographic claim.

## Final provisioning and identity decisions

macOS identity uses getentropy (SDK availability macOS 10.12+); Linux uses
getrandom with GRND_NONBLOCK. Linux uninitialized entropy returns EAGAIN;
missing syscall support and other source errors propagate without creating files.
No time/PID/random-library fallback exists. The 128 random bits are probabilistic
identity, not a mathematical uniqueness guarantee. Independently copied writable
pairs retain identity; do not mix their members. Backups must copy both files
while closed; snapshot coordination and copy identity regeneration are out of scope.

The source is documented by [Linux getrandom](https://man7.org/linux/man-pages/man2/getrandom.2.html)
and Apple's installed SDK sys/random.h declaration. The identity failure seam is
exercised deterministically. No SQLite-specific behavior was needed or adopted.

Admitted filesystem families: Linux ext-family, XFS and Btrfs by fstatfs type;
macOS APFS and HFS+ by fstatfs name. Others return ENOTSUP, including the overlay
filesystem of the tested Linux container. This allowlist limits applicability;
it does not certify arbitrary mount options, kernel versions or devices. No
network, volatile, overlay or other unrecognized filesystem can silently succeed.

For each member on EVERY successful open (new or existing): open its parent,
compare device/inode and regular-file type using fstat and fstatat with
AT_SYMLINK_NOFOLLOW, check filesystem, sync file, fsync directory, and recheck
association before closing directory. macOS adds file F_FULLFSYNC after directory
fsync to request device-cache drainage for preceding writes. This ordering is
an implementation inference from Apple's fsync/F_FULLFSYNC documentation, not an
Apple certification of this database. Provisioning close failure returns an error;
primary errors survive cleanup. Final symlinks and replacement inodes are rejected.

Linux [fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html)
specifies separate directory syncing and fdatasync's size-metadata behavior.
Apple [fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)
describes ordering and the device-cache limitation addressed by F_FULLFSYNC;
its [fcntl documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)
also qualifies device support. Failures propagate; there is no weaker fallback.

Ancestral directory entries must already be durable. The application owns stable,
trusted namespace ancestry until close, including any ancestor symlinks. This
feature does not create/sync directory trees or protect against concurrent renames.
Reopening alone is never evidence of prior provisioning. Only verified opened
resources and successful current sync operations establish this protocol's success.
Storage must honor flushes/order and preserve previously synced bytes when later
writes touch the same sector; power-safe overwrite behavior is an assumption,
not supplied by these syscalls. Arbitrary external truncation, media corruption
or lying hardware are not recoverable interruption scenarios.

Windows: [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
does not establish the required name durability through the existing file handle.
No volume flush/admin requirement or weaker mode was introduced. Identity creation
and provisioning return ENOTSUP; new batch creation fails before creating files,
and otherwise-valid existing pairs cannot open successfully. Invalid files may
still report their validation error first. Existing UTF-8 native file I/O and v1
lifecycle remain supported as before. Windows durable batch support is unresolved
platform work, separate from the feature 005 checkpoint format.

## Final creation, append and recovery boundaries

Creation opens/probes the WAL without modifying it, obtains identity, exclusively
creates DB then WAL, and writes both headers. A pre-existing WAL when DB creation
is requested returns PAIR_MISMATCH; no new DB is created. DB exclusive-create
EEXIST during open-or-create permits one existing-open attempt, then validates the
winner's pair. Another creator's empty or incomplete files fail immediately;
there is no wait/repair. WAL creation races fail without touching that winner.
DB creation followed by WAL failure may leave an empty DB. Neither missing member
is regenerated for an existing DB. All validation precedes provisioning or recovery
truncation. No failure path unlinks any file.

Append validates pointers/count/scratch/capacity first. It marks the handle failed,
writes body, syncs body, writes commit, syncs commit, then publishes end/sequence
and clears failed state. A successful append commits N records atomically under
the stated storage assumptions. Sequence output changes only on success. Capacity
failure cannot change file bytes or poison the handle; sequence overflow is checked
explicitly even though the fixed WAL bound makes it unreachable in ordinary use.
There are no buffered/relaxed modes. Sync failures never allow a same-handle retry.

| Interruption/result | Reopen behavior |
| --- | --- |
| No new bytes | Previous committed prefix |
| 1..31 bytes of new header | Ambiguous framing: fail closed, preserve files |
| Complete valid header, EOF before declared commit end | Uncommitted suffix: truncate to prior boundary, then sync |
| Body sync success, commit absent/short at EOF | Same recoverable incomplete suffix |
| Fully present frame with damaged header/body/commit or sequence gap | Fail closed, never truncate |
| Complete valid commit before failed/missing final sync | Batch may be present or absent; validate actual storage |
| Final sync succeeds; success return lost | Batch is committed under contract; caller remains uncertain |
| Append success acknowledged | Batch persists under contract and is read whole after reopen |
| Recovery truncate fails before/after effects | Open fails; close resources; next open rescans actual size |
| Truncate succeeds, recovery sync fails/interrupted | Old incomplete suffix or cleaned boundary may return; both rescan safely |

An incomplete suffix is provable only within the interruption/storage model:
validated framing establishes a batch with no complete commit in the file. A
checksum failure is never interpreted as incomplete framing. Deliberate/external
truncation of a committed batch is indistinguishable from an incomplete append;
such modification is excluded by serialized ownership. Fully present bad framing
is ambiguous even at EOF and is not repaired. A torn header can prevent opening
previously committed data until an external investigation; no repair tool is added.

Recovery scans before publishing ownership, validates every complete batch, then
re-provisions both files. Only afterwards does it truncate a validated incomplete
suffix and sync. It never truncates an earlier committed boundary. Recovery has
no write-ahead cleanup marker to tear: interrupted shrink can leave the old tail
or the exact previous boundary under the storage assumptions. Malformed outcomes
still fail closed. Closing consumes both handles once; first error wins.

Reader output is decoded from scratch only after full frame validation. Count,
sequence, cursor and output remain unchanged on any error, insufficient output
capacity, or END. Scratch may change. Reads have a single handle-local cursor,
start at sequence 1, and can see subsequent serialized successful appends after
END. Rewind resets it. Failed append blocks next/rewind/append until close/reopen.
After reopen, sequences count recovered commits; discarded attempts can reuse a
sequence, so a sequence is not an attempt identifier. Reconciliation uses ordered
sequences plus content and caller context, without exactly-once guarantees.

## Actual implementation and verification

Added v2 core/API, narrow POSIX identity/provision operations and explicit Windows
unsupported implementations; retained all v1 lifecycle and native backend tests.
Added shared tests/batch_test.c, deterministic tests/batch_model_test.c and POSIX
provisioning syscall tests. Make includes the new tests and example; existing
Linux/macOS and x86 32-bit CI inherit them, Windows CI adds batch native rejection
and model tests while preserving every original build/run. README contains the
complete example plus memory, capacity, errors, compatibility and durability rules.
AGENTS.md and CLAUDE.md now describe the accepted architecture.

The model uses separate volatile/persisted bytes and namespace state, persists
only dirty ranges on successful sync, and can lose unflushed writes on crash.
It also explicitly lets partial unsynced writes persist. It checks all four append
write/sync boundaries before/after effects, previously acknowledged batches,
uncertain present/absent commits, every short body write (including zero), recovery
truncate before/after failure and sync failure, header creation prefixes, read and
size failures, primary/close ownership, creation failures/race, format/association
errors, independent golden encoding, and complete 64 MiB exhaustion/reopen.
The model is deterministic test code with static arrays; this memory is not in
the library. It assumes synced ranges and names persist, atomic modeled length
selection, stable ownership and no damage to previously persisted bytes. It is not
an emulator of filesystem journaling, physical sectors, controllers or power cuts.

Native batch tests cover append/read/reopen, boundary values, small buffers, END,
v1 policy, every second-batch truncated prefix and single-bit corruption throughout
two committed frames. Native provisioning tests inject failure at each internal
operation, including path replacement before/after flush, unsupported filesystem,
and entropy failure. Existing backend fault tests retain partial syscall progress,
zero writes, EINTR where applicable, errors and Windows UTF-8 coverage.

Verification results are recorded below; no commit, push or merge is authorized
or performed. Feature 005 must add a separately designed
main-storage checkpoint/install protocol and safe WAL reclamation, without
invalidating the only committed copy. No checkpoint structures are frozen here.

### Checks actually performed

| Environment / command | Result |
| --- | --- |
| macOS ARM64, Darwin 25.6.0, Apple Clang 21.0.0; make check, strict C99 -Wall -Wextra -Wpedantic -Werror | Passed, including real batch recovery, native provisioning faults and all legacy tests |
| Same host; make clean then make check with CFLAGS='-std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined' | Passed; final core and expanded model tests passed with sanitizers |
| Same host; make clean then restore default make check | Passed |
| build/batches DATABASE WAL in a private temporary directory | Passed: printed batch 1, series 7, timestamp 1700000000000000, value 23500 after close/reopen; DB size 32, WAL size 116 |
| Disposable gcc:14 Linux container, source mounted read-only and copied to /tmp; make clean then make check | Passed strict builds, legacy tests, provisioning faults and batch model; native batch test explicitly reported unsupported provisioning on overlay FS |
| Same container; x86_64-w64-mingw32-gcc strict C99 compile/link | Passed examples/basic.c and examples/batches.c with Windows backend, lifecycle_test.c and batch_test.c with Windows backend, and batch_model_test.c with fake backend |
| git diff --check and git diff --cached --check | Passed |

The simulated interruptions are the deterministic cases described above, not
physical process/power-cut experiments. Linux's admitted ext-family/XFS/Btrfs
batch runtime has not been exercised here; the container rejected overlay as
intended. The MinGW builds were compiled/linked only, not executed, and do not
validate Clang plus Microsoft SDK behavior. Windows runtime, GitHub Actions,
Linux x86 32-bit runtime, ARM32 and physical target devices are unrun for this
feature. Exact deployment CPU/OS/ABI and storage configuration remain unconfirmed.
No hardware power-loss, filesystem corruption injection, or controller flush
validation has been performed. Passing the model is not proof of hardware safety.

The index was clean initially. During work, the initial feature note and native
batch test became staged externally; no agent command staged or unstaged files.
Their observed staged diff SHA-256 remained
`db91f29243f0f676a0efc32313a195a726946dd295752242a4ffee86ca6732b6`.
Final documentation additions are unstaged relative to that initial note.
No commits, pushes, merges or deletions of user files were performed.
