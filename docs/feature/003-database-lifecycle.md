# 003: Database lifecycle and storage contract

Status: implemented; local macOS/Linux tests and Windows cross-builds passed.
Windows runtime/SDK, CI, Linux x86 32-bit and target-device validation pending.
Branch: codex/003-database-lifecycle from freshly fetched origin/main 0e70eb4.
Feature 002 commit 9f7884f is an ancestor of origin/main. Initial working tree and
index were clean. Research was removed upstream; no research files were changed.

## Accepted scope and implementation

C99, caller-owned handle, no allocations, one owner and serialized calls. Implement
init, existing open, exclusive create, explicit open-or-create, validation and close
in timelite.c/h. Native operations stay in the existing file layer. No record or
WAL operations, locking, queries, or concurrent/multi-process safety.

Public struct contains only private native-resource storage (int on POSIX, void *
on Windows); transfer it through a local internal file struct without aliasing or
exposing file_io.h. No stable binary ABI across builds/platforms is promised.
Initialization writes a closed state without reading uninitialized memory. It is
only legal on fresh or closed storage; initializing an open handle would lose its
resource. Open handles must not be copied, modified, or closed externally.

Final API: timelite_init(db), timelite_open(db, path, mode), timelite_close(db).
Modes: TIMELITE_OPEN_EXISTING, TIMELITE_CREATE_NEW, TIMELITE_OPEN_OR_CREATE.
Return 0, positive errno values from the backend, or negative format errors:
TIMELITE_INVALID_DATABASE (-1), TIMELITE_UNSUPPORTED_VERSION (-2).
NULL/empty path, NULL handle, invalid mode or open on an open handle: EINVAL.
Close of a closed initialized handle: EBADF. Uninitialized/modified handles and
invalid C pointers are caller contract violations, not detectable input errors.

## Header and validation

Exactly 12 bytes; no padding or native structs:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | ASCII `TIMELITE` (54 49 4d 45 4c 49 54 45 hex) |
| 8 | 4 | unsigned format version, little endian; current value 1 |

Explicit byte encoding/decoding. Validation order: read 12 bytes, require complete
header, check identifier, decode version (zero invalid; any nonzero version other
than 1 unsupported), then require file size exactly 12. Thus an unknown version
is reported before its size is interpreted. Empty/short/unrelated/zero-version
files and v1 trailing data are invalid. No reserved fields, checksum, record
layout or database identity yet. Exact constants detect malformed headers, not
arbitrary corruption transformed into another valid header. Adding records or WAL
requires an explicit version decision; no automatic migration or future-version
compatibility. A complete valid header is the on-disk initialization marker.

## Operation-by-operation failure and ownership contract (before coding)

| Operation / failure point | File effects | Handle / returned error |
| --- | --- | --- |
| init fresh/closed storage | None | Closed; NULL returns EINVAL |
| argument rejection | None | Existing state unchanged; EINVAL |
| existing open fails | Never creates or truncates | Closed; backend error |
| exclusive create fails | Existing path untouched; post-native-open backend failure may leave empty new file | Closed; primary backend error |
| open-or-create | Try existing open; only ENOENT triggers exclusive create | Other errors returned unchanged |
| another creator wins | EEXIST from create triggers exactly one existing open and validation | Never overwrite; absent again returns ENOENT; incomplete winner is rejected without waiting |
| header read/size fails | No writes | Close once; preserve primary I/O error |
| header validation fails | No writes | Close once; format error preserved |
| new header write fails / reports short success | Empty, partial, or even complete header can remain | Close once; write error or EIO for short success |
| new file sync fails | Complete header can remain and may reopen successfully | Close once; sync error preserved |
| initialization succeeds | Complete header write and successful file sync | Owns open resource; returns 0 |
| cleanup close also fails | Never unlink, truncate, or retry cleanup | Caller remains closed; primary failure wins; OS resource release uncertain |
| explicit close succeeds/fails | No implicit sync or rollback | Consumes resource once even on error; reusable closed handle; close error returned |

Only a successful create followed by full write and file sync returns initialized
success. Reopen checks observable header validity, not whether a past sync or API
return succeeded. An incomplete header is rejected, never repaired. An interruption
after the final byte may leave a valid file even if the caller never saw success.
No cleanup deletes a path, including one originally created here: path ownership
could have changed. There is no transaction-like creation rollback.

## Creation durability and official sources

Reviewed for this feature: Linux [fsync/fdatasync](https://man7.org/linux/man-pages/man2/fsync.2.html)
requires a separate directory fsync for a durable directory entry. File data and
metadata needed to retrieve it are covered by fdatasync. Apple
[F_FULLFSYNC](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)
requests device-cache flushing, with device limitations. Windows
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
flushes buffered information for the open file; this feature does not flush a
volume or directory. These requests are not interchangeable platform guarantees.

Decision: use existing file sync, propagate failures, add no directory operation.
Successful creation guarantees initialization and ordinary reopen visibility;
it does NOT promise the name or contents survive power loss. Filesystem, device,
and OS behavior remain relevant. Reopen tests prove visibility only. This limit
must be resolved before claiming durable future appends to newly created files.

## WAL: intended next-feature contract, specification only

Historical note (2026-09-12): feature 004 implemented the WAL append and
recovery below; feature 006 ([006-checkpoint.md](006-checkpoint.md))
implemented the checkpoint/install protocol and extends the interruption table.

Accepted: WAL is the default and initially only future write mode. A WAL is a
separate file that first records a complete batch before that batch is incorporated
into main storage. The first append API should durably commit a whole batch before
returning success. Buffered/relaxed modes are deferred. Nothing below implements
append, recovery, checkpointing, or a WAL binary format.

Proposed: log batches of sensor records, not page images. Main storage would hold
immutable encoded sensor segments plus recoverable metadata identifying installed
segments and the last incorporated batch. Earlier append-only segment research is
reconciled as the destination of checkpointing, not the WAL itself. An append-only
log used forever as the only data store would be a different architecture: it would
not have this transfer-and-reclaim WAL lifecycle. Segment layout is deferred.

Proposed batches carry bounded lengths, record count, monotonic batch sequence,
and integrity validation covering framing and contents. A final commit record
binds those values and the complete batch. Recovery scans in order, validates
bounds before reading, and exposes only whole validated committed batches.
Checksums detect accidental damage, not malicious tampering. Exact algorithms,
limits, framing, and binary encoding remain to be designed and failure-tested.

Example: append 100 temperature readings as batch 42. Write all framing and records,
then its commit record, then sync the WAL. Return durable success only after that
sync AND the required database/WAL identities, names, and metadata are durable.
Errors leave commit outcome uncertain: bytes can reach storage before an error or
a lost return. Recovery may therefore find batch 42 even if append failed. Future
callers need reconciliation or an explicit deduplication design before retrying;
exactly-once retries are not promised.

| Interruption point | Intended recovery behavior |
| --- | --- |
| Before any batch bytes | Previous committed batches only |
| Mid-batch or incomplete final commit record at EOF | Ignore only a provably incomplete trailing batch; retain prior commits |
| Complete commit written, before/during failed sync | Batch may survive fully or not; validate actual storage, never infer from API result |
| Sync succeeded, before success reaches caller | Batch present under the qualified durability contract; caller may be uncertain |
| Success returned, before checkpoint | Read batch from WAL plus already installed main segments |
| Main segment being written | WAL remains authoritative; incomplete uninstalled segment is not visible |
| Main segment synced, installation metadata not durable | Replay WAL; ignore orphan segment using a defined recovery protocol |
| Installation metadata durable, before WAL reclaim | Sequence boundary prevents duplicate replay; redundant WAL is safe |
| During reclaim | Already installed data must remain readable; reclaim protocol itself must recover safely |

A fully present frame with bad integrity, impossible length, wrong identity,
sequence gap, invalid commit reference, or damage within the committed prefix is
corruption to report, not a license to discard history. An incomplete tail can be
ignored only when validated prior framing establishes it as an uncommitted suffix.
Ambiguous torn/corrupt cases fail closed. The future format must make this
distinction supportable; no recovery guarantee exists until then.

Reads before checkpoint combine main storage with committed WAL batches. A bounded
scan can provide this initially; no unbounded in-memory index is required.
Checkpoint writes new immutable segments, syncs them, durably installs recoverable
metadata and directory changes, and only then reclaims covered WAL space. In-place
metadata overwrites cannot be assumed atomic. A generation-based install protocol
and restart validation must be designed; partial checkpoint completion cannot
invalidate the only committed copy. Sequence numbers avoid duplicate exposure.

Proposed association: generate a database identity and WAL generation when WAL is
introduced; store and validate both in the relevant versioned files. Matching
filenames alone is insufficient. Reject mismatches without deleting either file.
Identity generation, collision policy, copy/backup behavior and format migration
are deferred; no speculative identity fields are added in v1.

Before durable append is implemented, establish platform-specific durable creation
of the database, WAL, new segments and install metadata, including directories.
Precreating reusable WAL storage may reduce namespace operations but does not
remove initial provisioning requirements. Rotation, rename and deletion need an
explicit durable ordering protocol. Unsupported storage must fail the future
durability operation rather than silently downgrade success. Current lifecycle
success alone is insufficient provisioning evidence.

Use caller-owned batch input and bounded scratch buffers; define hard byte/count
limits and overflow checks. Stream validation and recovery with bounded memory;
reject oversized batches before effects. One owner serializes append, reads,
checkpoint and close; no threads, locks, allocation or multi-process support.

SQLite reference: [official WAL description](https://sqlite.org/wal.html) explains
commit markers, reading committed WAL content, and checkpointing back to the main
database. Timelite borrows that ordering idea and embedded-library experience;
SQLite's page frames, shared-memory index, concurrency and SQL are not adopted.

## Verification plan

Shared real-backend tests: exact header bytes, create/open modes, close/reopen,
invalid arguments and repeated operations, malformed/version/length rejection and
byte preservation, every incomplete initialization prefix, Windows Unicode path.
Separate test executable links timelite.c against a deterministic fake file
boundary: race, read/write errors and short results, size/sync/close failures,
primary error preservation, bounded attempts and consumed handles. Existing native
fault tests continue to cover short syscall transfers and EINTR behavior.
Run strict make check, sanitizer checks, available Linux and Windows cross-builds;
CI extends all existing platforms. Report runtime, cross-compile, CI, and target
results separately. No power-cut proof or device support claims.


## Actual changes and verification results

Implemented the API and 12-byte header described above in timelite.h/timelite.c.
The handle holds a single native resource; temporary internal structs move that
resource without pointer casts or exposing internal file operations publicly.
Arguments are checked before effects. Existing-file validation performs no writes
or syncs. Open-or-create has bounded retry behavior. Failure cleanup preserves the
original error and never deletes a file. No production backend changes were needed.

Added tests/lifecycle_test.c against real backends and tests/lifecycle_fault_test.c
against a deterministic in-memory backend linked only into that test executable.
The latter is allocation-free and does not access real files. It simulates complete,
incomplete, and disappearing race winners; read errors/short results, write
errors/short success including every incomplete prefix, size errors, sync failure,
cleanup close failure, handle reuse, and valid artifacts after failed write/sync.
The shared integration tests check exact encoding (including all four version
bytes), all three modes, byte preservation and incomplete initialization artifacts.
Windows uses a Unicode filename through the existing backend. Shared Windows
private-directory setup/cleanup was extracted into tests/windows_test_paths.h;
existing backend tests retain the same helper behavior.

make check now runs both lifecycle tests as well as the existing example and
file-I/O tests. Linux/macOS and Linux x86 32-bit CI inherit these targets; Windows
CI compiles and runs both new executables explicitly and retains its public-library
smoke build and backend tests. Public direct linking commands include one backend.
README includes a lifecycle example; AGENTS.md and CLAUDE.md describe the accepted
lifecycle/WAL direction and build requirements. No append or WAL code was added.

Checks actually performed:

| Environment / command | Result |
| --- | --- |
| macOS ARM64, Darwin 25.6.0, Apple Clang 21.0.0: make check | Passed, default strict C99 warnings; all five programs |
| Same host: make clean then make check with CFLAGS='-std=c99 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined' | Passed |
| Same host: clean and restore default make check | Passed |
| README direct public version build and run | Passed |
| README C lifecycle example, compiled strictly and run twice in a private temporary directory | Passed create/reopen and exact byte check |
| Disposable gcc:14 Linux AArch64 container: read-only source copied to /tmp, make clean then make check | Passed all programs with strict C99 warnings |
| Same container: x86_64-w64-mingw32-gcc (reports GCC 14-win32), strict C99 cross-compilation | Passed public example with Windows backend, both lifecycle executables, shared file-I/O integration and Windows fault executable |
| git diff --check | Passed |

Windows executables were compiled/linked only, not executed. MinGW headers are
not Microsoft SDK validation. Windows Server 2022 x64 Clang/SDK runtime and the
updated GitHub Actions jobs have not run for this feature. Linux x86 32-bit is
configured in CI but unrun locally. No physical target device, ARM32, filesystem
fault injection under power loss, or power-cut testing was performed. Exact
deployment CPU, OS and ABI remain unconfirmed. No commits, pushes or merges were
performed. Existing index contents were not changed by agent commands.

## Remaining limits

Creation is non-transactional and does not establish directory durability. A full
header is an observable validity marker, not evidence of a successful past API
return or durable creation. File sync failures can leave valid databases. Native
close failures consume the library handle but leave OS release uncertain. The
minimal header cannot distinguish separate databases or support WAL association;
identity and format evolution must be designed before WAL implementation. Neither
opening nor creation supplies locking or protection against external modification.
Only trusted paths and serialized ownership are supported. Future durable batch
success depends on resolving provisioning/directory durability and implementing
the proposed commit/recovery/checkpoint protocols with failure tests.
