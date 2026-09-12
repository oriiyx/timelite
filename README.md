# Timelite

A small embedded time-series database project in C99. The current version is a
library with durable sensor batch append, recovery, and sequential reading from a
separate WAL on supported local POSIX filesystems. The original v1 lifecycle API
remains available. Windows retains lifecycle and file I/O; durable batch
provisioning currently returns `ENOTSUP`. Checkpointing remains deferred to a later
feature; feature 005 is the repeatable testing suite described under Testing.

The direction is a small C API, explicit memory ownership, no third-party
dependencies, and a narrow scope for sensor history. Target devices include x86
and ARM32; device support has not been verified yet.

## Build

On Linux or macOS, install a C99 compiler, Make, ar and Python 3 (used only by
the test runner, never by the library), then run:

```sh
make check
```

This builds build/libtimelite.a, build/basic and build/batches with Make, then
runs the native test profile through the runner (see Testing). The example prints:

```text
Timelite 0.1.0-dev
```

Use `make clean` to remove all build output, including test reports. To select
another compiler or flags, use `make CC=clang check` or `make CFLAGS=-O2 check`;
Make records the toolchain and flags in build/flags and rebuilds its outputs
automatically when they change.

You can also compile the library directly into an application:

```sh
mkdir -p build
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c file_io.c examples/basic.c -o build/basic-direct
./build/basic-direct
```

On Windows x64, install Clang with llvm-ar and the Windows SDK (including the
Visual C++ build tools) and Python 3, then run in PowerShell:

```powershell
python tools/test.py run windows-native
```

This builds the static library with `clang --target=x86_64-pc-windows-msvc`
and runs every Windows-applicable test: the public examples, file I/O, Windows
API fault, lifecycle and batch tests. Native batch provisioning is expected to
return `ENOTSUP` there, and the suite records that as the verified contract. A
direct public-library smoke build is still one command:

```powershell
clang -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c file_io_windows.c examples/basic.c -o build/basic.exe
```

## Testing

Verification is routine: register a test once, run a documented command and
read a summary. The suite is a Python 3 standard-library runner
([tools/test.py](tools/test.py)), one explicit inventory
([tools/inventory.json](tools/inventory.json)) shared with CI, and a pinned
Docker toolchain image ([tools/docker/Dockerfile](tools/docker/Dockerfile)).
Production code and all behavior, fault and model tests stay in C99.

### Quick start

```sh
make check                                  # fast native check while iterating
python3 tools/test.py list                  # groups, profiles, inventory
python3 tools/test.py doctor                # which tools and images are available
python3 tools/test.py run local             # native + sanitize + runner self-tests
python3 tools/test.py prepare               # build the Docker image once (network)
python3 tools/test.py run preflight         # local + linux + linux32 + windows-cross
python3 tools/test.py run native --test batch
python3 tools/test.py run linux32 --jobs 4 --timeout 300
python3 tools/test.py run native --test-root /mnt/ext4-scratch
```

`run` takes one profile or group, `--test NAME` for exactly one inventory
entry, `--jobs N` (1..8, default 2) for parallel tests within a configuration,
`--timeout SECONDS` per command (default 120), `--test-root DIR` for storage
tests on a chosen filesystem, and `--output DIR` for a new run directory.
Ctrl-C stops owned child processes and containers and still writes reports.

### Profiles and groups

| Profile | Where | What |
| --- | --- | --- |
| `native` | host | host compiler, strict C99, every applicable test executed |
| `sanitize` | host | same with AddressSanitizer and UndefinedBehaviorSanitizer |
| `runner` | host | unit tests of the runner itself ([tools/test_runner.py](tools/test_runner.py)) |
| `linux` | Docker | Debian gcc, x86-64 |
| `linux32` | Docker | Debian gcc `-m32`, x86 32-bit with 64-bit file offsets |
| `windows-cross` | Docker | MinGW-w64 x86-64 cross-compilation only, nothing executed |
| `windows-native` | Windows host | Clang with the Windows SDK, tests executed |

`local` is exactly `native + sanitize + runner`. `preflight` is `local` plus
`linux + linux32 + windows-cross`. A profile whose tools are missing is reported
BLOCKED and fails the group; nothing is silently dropped. Container profiles
always request `linux/amd64`; on an ARM64 host they run under emulation and
the report says so. A container result is never an ARM32 or device result.

### Preparation

`prepare` builds the image from the checked-in Dockerfile. The base image is
pinned by digest; package versions come from the Debian repositories at build
time and are recorded in /etc/timelite-toolchain.txt inside the image, printed
by `prepare` and stored in every container report. The tag is derived from the
Dockerfile hash, so editing it requires another `prepare`. Prepared runs use
`--network=none`, mount the source snapshot read-only and write only to the run
directory (plus a private child of `--test-root` when given). No privileged
containers or host devices are used.

### Results

Statuses are PASS, FAIL, SKIP, BLOCKED and NOT RUN, each with a reason. A run
passes only when at least one test passed and every other row is a PASS or an
optional SKIP. Required skips, BLOCKED, NOT RUN, timeouts, missing tools, an
empty selection and a source change during the run all fail the run. Test
programs use fixed exit codes: 0 pass, 77 durable provisioning unsupported on
the storage, 78 required group not run (the >4 GiB sparse group); anything else
is a failure. Failures name the test case and the injected boundary, not only
an assertion line.

Coverage is separated explicitly. `storage_probe` reports whether the run's
storage supports durable provisioning. On supported storage the `batch` test is
recorded as "native batch behavior exercised"; on Windows the `ENOTSUP` result
is the verified contract; in containers an unsupported overlay or shared
filesystem records an optional skip labelled "native batch behavior NOT RUN",
which is not durability evidence. The `native` and `sanitize` profiles require
supported storage: pass `--test-root` with a directory on a supported local
filesystem when the default location is not one. Model interruptions, native
runtime, cross-compilation, CI, device and physical power-cut testing are
reported separately; the suite covers the first four.

### Artifacts and failure diagnosis

Every run creates a unique directory `build/test-runs/run-<time>-<id>/` with
`source/` (the bounded snapshot every profile built from), one folder per
profile holding binaries and one `.log` per command, `report.json` (source
digest, Git commit and dirty files, host and target architecture, compiler
version, image identity, exact commands, timings, exit codes) and `junit.xml`.
On failure the summary prints the failing command, its log path and an exact
rerun command such as `python3 tools/test.py run native --test batch`. Failing
tests keep their private storage directory. Nothing outside the run directory
is written or cleaned; `make clean` removes all of build/.

### Adding tests

- New C test: add `tests/<name>_test.c` and one entry to
  [tools/inventory.json](tools/inventory.json) with `name`, `source`,
  `backend` (`library` links build output libtimelite.a; `native` compiles the
  test with one native backend; `fake` compiles it with timelite.c only;
  `none` compiles the test alone), optional `defines`, `platforms`
  (`posix`, `windows`), a `coverage` sentence, optional `execution`
  (`batch`, `batch-example`, `probe`) and optional expected `stdout`. Nothing
  else changes: Make, CI and the runner all read the inventory.
- New case in an existing test: add it to the C file and wrap the loop or
  step in `TEST_CASE(name, boundary)` from tests/test_assert.h so failures
  report which case and boundary broke. No runner change is needed.
- Runner behavior: add a unit test to tools/test_runner.py; it runs in the
  `runner` profile.

### Continuous integration

[.github/workflows/build.yml](.github/workflows/build.yml) calls the same
runner and profiles: `local` on Ubuntu and macOS plus `make check`, the three
container profiles on an Ubuntu x86-64 runner after `prepare`, and
`windows-native` on Windows Server 2022 x64 with Clang and the SDK. Every job
uploads build/test-runs even when it fails and has a bounded timeout. CI
results are separate from local runs and from device or power-cut testing.

### Stopping rule

When the required checks pass for the relevant source and that source has not
changed, stop. Rerun only after a change, a failure or for a concrete coverage
gap. An ad hoc command that provides reusable coverage belongs in the inventory
or runner, not in a chat transcript.

## Durable sensor batches (v2)

The complete example below is also [examples/batches.c](examples/batches.c).
Build it with `make`, then run `./build/batches DATABASE WAL` using two unused
paths in an existing, stable, already-durable local directory. It creates a pair,
appends one batch, closes, reopens, and reads the committed batch. It leaves both
files in place. On unsupported provisioning, including Windows, creation returns
an error; partial artifacts may remain and are never automatically deleted.

```c
#include "timelite.h"
#include <stdio.h>
#include <inttypes.h>

/* Supply two unused paths in a stable, already-durable local directory. */
int main(int argc, char **argv)
{
    struct timelite_batches db;
    struct timelite_record input[] = {{7, UINT64_C(1700000000000000), 23500}};
    struct timelite_record output[TIMELITE_MAX_RECORDS];
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    uint64_t sequence;
    size_t count, i;
    int error, close_error;
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s DATABASE WAL\n", argv[0]);
        return 1;
    }
    (void)timelite_batches_init(&db);
    error = timelite_batches_open(&db, argv[1], argv[2], TIMELITE_CREATE_NEW,
                                  scratch, sizeof(scratch));
    if (error != 0)
    {
        fprintf(stderr, "create failed: %d (partial files may remain)\n", error);
        return 1;
    }
    /* Application convention: series 7 measures thousandths of a degree C. */
    error = timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence);
    close_error = timelite_batches_close(&db);
    if (error != 0)
    {
        fprintf(stderr, "append failed: %d; reopen and inspect before retrying\n", error);
        return 1;
    }
    if (close_error != 0)
    {
        fprintf(stderr, "close failed: %d; handle consumed, do not retry close\n", close_error);
        return 1;
    }
    error = timelite_batches_open(&db, argv[1], argv[2], TIMELITE_OPEN_EXISTING,
                                  scratch, sizeof(scratch));
    if (error != 0)
    {
        fprintf(stderr, "reopen failed: %d\n", error);
        return 1;
    }
    while ((error = timelite_batches_next(&db, output, TIMELITE_MAX_RECORDS,
                                          &count, &sequence, scratch,
                                          sizeof(scratch))) == 0)
    {
        for (i = 0; i < count; i++)
        {
            printf("batch=%" PRIu64 " series=%" PRIu32 " us=%" PRIu64
                   " value=%" PRId64 "\n", sequence, output[i].series,
                   output[i].timestamp_us, output[i].value);
        }
    }
    close_error = timelite_batches_close(&db);
    if (error != TIMELITE_END || close_error != 0)
    {
        fprintf(stderr, "read/close failed: %d/%d\n", error, close_error);
        return 1;
    }
    return 0;
}
```

Each reading has a 32-bit series identifier, unsigned 64-bit microseconds since
the Unix epoch, and signed 64-bit integer value. The application chooses units
(e.g. thousandths of a degree); no floating-point representation is assumed.
All field values are valid, including series zero. Input order and duplicate
series/timestamps are preserved, with no ordering requirement or deduplication.
Empty batches and more than 64 readings return `EINVAL` before writes.

Caller owns the handle, records and scratch. Every batch operation takes at least
`TIMELITE_BATCH_SCRATCH` (1344) scratch bytes. Output capacity is in records;
64 records always suffice. Public struct size can include platform padding;
encoded records are exactly 20 bytes. No allocation, retained buffers, global
mutable production state, threads, or locks. Buffers/outputs and handles must not
overlap. Calls and filesystem ownership must remain serialized; do not copy an
open handle or externally modify, rename, replace or delete its files/directories.

The WAL limit is 64 MiB including its 32-byte header. A batch occupies
64 + 20 × record count bytes (84..1344). `TIMELITE_WAL_FULL` rejects a batch
before writes; committed data is never overwritten or reclaimed. A later
checkpointing feature must move batches into main storage before reclaiming WAL
capacity. This feature
provides no rotation, retention, segments, indexes or queries beyond batch reading.

`timelite_batches_next` returns only whole validated batches. `TIMELITE_END`
means the cursor reached currently committed data; later serialized appends are
visible at that cursor. `timelite_batches_rewind` restarts at sequence 1.
`TIMELITE_BUFFER_TOO_SMALL` leaves cursor and output unchanged, allowing retry
with larger buffers. Errors and END leave count, sequence and record outputs
unchanged; scratch contents are unspecified. A successful append returns the
next sequence, starting at 1. Reopen derives that sequence from validated commits.

Positive returns are errno values; negative returns are Timelite results declared
in [timelite.h](timelite.h). Argument/buffer/capacity rejection leaves the handle
usable. Any append write or sync error poisons reads and appends with
`TIMELITE_RECOVERY_REQUIRED` until close/reopen. The failed batch may still be
committed: enumerate sequences and contents after reopen before deciding to retry.
Identical contents cannot distinguish independent identical readings from a retry;
there is no exactly-once promise. Close consumes both resources even on failure;
never retry native close. Cleanup preserves the primary error and deletes nothing.

Creation requires both names to be unused. Existing opens require both members;
open-or-create never repairs a missing member of an existing pair. Partial
creation may leave empty, partial or complete files. v2 headers associate the pair
using an OS-random 128-bit identity and integrity checks. Different identities
return `TIMELITE_PAIR_MISMATCH` before modification. Copy/backup both files only
while closed; independently writable copies must not be mixed. Random identity
collision is improbable, not mathematically impossible or tamper protection.

The v1 12-byte format and `timelite_open` API retain their original behavior.
The v2 API rejects v1 with `TIMELITE_UNSUPPORTED_VERSION`; the v1 API likewise
rejects v2. Migration is deferred; no automatic upgrade or file deletion occurs.

Durable success requires a local filesystem supporting the specified flush
operations: macOS APFS/HFS+ or Linux ext-family/XFS/Btrfs are admitted; other types
return `ENOTSUP`. This is a protocol boundary, not certification of every mount,
OS version or device. Linux uses file `fdatasync` and parent-directory `fsync`;
macOS uses directory `fsync` followed by file `F_FULLFSYNC` to request device-cache
flushing. Every open repeats provisioning and checks the final directory entries
against the opened files. Final-component symlinks are rejected. Directory
ancestry must already be durable, and paths must remain stable until close.
Unsupported/failed flushes return errors without a weaker fallback. Windows has
no implemented namespace durability protocol, so valid batch opens/creation
cannot succeed there yet; existing file-I/O and v1 operations still work.

Storage must honor flushes and ordering, preserve previously synced bytes during
later writes (including writes to the same physical sector), and preserve the
synced namespace. No universal power-loss guarantee follows from an OS flush.
Recovery checks complete framing, sequence and checksums, then truncates/syncs
only a suffix with a valid header declaring a batch beyond EOF. A short or damaged
header, complete bad commit, or damaged committed payload fails closed. An
interrupted append can therefore require manual investigation if its header is
partial. Arbitrary external truncation is outside the recovery model: it cannot
be distinguished from an interrupted uncommitted suffix. Checksums detect
accidental damage with finite collision probability, not malicious edits.

See [feature 004](docs/feature/004-durable-batch.md) for exact encoding, failure
boundaries, official durability sources, and separate verification results.

## Database lifecycle

Initialize caller-owned storage before using it. Open handles have one owner and
must not be copied or reinitialized. Calls must be serialized; there is no locking
or concurrent/multi-process safety. Link the platform backend as shown above.

```c
#include <stdio.h>
#include "timelite.h"

int main(void)
{
    struct timelite_db db;
    int error = timelite_init(&db);
    if (error == 0)
    {
        error = timelite_open(&db, "sensors.tl", TIMELITE_OPEN_OR_CREATE);
    }
    if (error != 0)
    {
        fprintf(stderr, "open failed: %d\n", error);
        return 1;
    }
    error = timelite_close(&db);
    return error == 0 ? 0 : 1;
}
```

`TIMELITE_OPEN_EXISTING` requires a valid existing database;
`TIMELITE_CREATE_NEW` creates exclusively; `TIMELITE_OPEN_OR_CREATE` opens or
exclusively creates when absent. Opening never truncates or repairs a file.
The current database is exactly a 12-byte identifier/version header, with no
records. Empty, short, malformed or v1 files with extra bytes return
`TIMELITE_INVALID_DATABASE`; nonzero unknown versions return
`TIMELITE_UNSUPPORTED_VERSION`. Other failures return positive errno values
(including mapped Windows errors). Invalid arguments return EINVAL.

Argument rejection leaves the handle unchanged. Other failed opens leave a closed
reusable handle. Failed creation may leave an empty,
partial or valid file; no cleanup deletes it. A valid file can reopen even after
creation returned a sync error. Close consumes the handle even on failure; native
resource release is then uncertain. Never retry close on the consumed resource.
Creation writes and syncs the header but does not sync the parent directory;
success does not promise durable creation under power loss. Reopen proves only
visibility. See [feature 003](docs/feature/003-database-lifecycle.md) for exact
validation order, ownership, format evolution and the proposed WAL contract.

The separate v2 batch API above implements WAL append and recovery. Checkpointing
and main-storage segments remain deferred to a later feature.

## Internal file I/O

[file_io.h](file_io.h) defines the internal contract; it is deliberately absent
from the public database header. The POSIX Make build includes file_io.c in the
archive. Windows builds select file_io_windows.c instead; compile exactly one
backend. The public library must link exactly one backend, even for the version example.

The layer opens regular files read/write without truncating, creates exclusively
with mode 0600 on POSIX (subject to umask), reads/writes at explicit offsets, obtains size,
truncates, syncs, and closes. Initialize a caller-owned `struct timelite_file` with
`TIMELITE_FILE_INIT`. Pass caller-owned buffers and byte-count outputs. Functions
return zero or an errno value (mapped from native errors on Windows). Reads stop successfully at EOF; writes finish
or return an error with the completed count. Errors do not roll back changes.
See the header for argument, ownership, range, and close-error rules.

Windows uses synchronous, non-inheritable native handles and checked UTF-8 to
UTF-16 conversion without allocation. Paths are limited to 259 UTF-8 bytes,
excluding NUL; bad encoding returns EILSEQ and excess length ENAMETOOLONG.
Normal Win32 path rules still apply; no long-path prefix is added. Use trusted
regular-file paths. Create inherits directory ACLs. Sharing permits read, write,
rename and deletion by compatible handles; other opens can deny sharing. These
flags provide no process locking. Transfers are capped at DWORD limits and all
ranges must fit INT64_MAX. Extension writes a final zero byte, using Windows'
documented zero-fill of the gap. Sparse allocation is not enabled automatically.
Close consumes the caller's handle even on failure; resource release is then
uncertain. No failed operation rolls back file changes. Full mappings and
platform differences are in [feature 002](docs/feature/002-windows-file-io.md).

Linux is the provisional deployment OS and macOS the local development OS.
Linux uses 64-bit file offsets, including in 32-bit builds. Exact x86/ARM32 CPU,
OS, and ABI still require confirmation. One owner must serialize calls; this
layer provides no process locking or protection from concurrent file changes.

Explicit sync uses Linux `fdatasync` or macOS `F_FULLFSYNC`, reporting failures
without a weaker fallback. Windows uses `FlushFileBuffers` to request flushing
buffered file information to the device, reporting failure without fallback.
There is no claim that these OS requests have identical durability semantics.
Ordinary file create/sync does not sync the parent directory. The separate internal
`timelite_file_provision` operation used by v2 checks association and syncs the
parent directory; ordinary creation alone has no durable directory-entry promise. Close is not
sync. These legacy file operations alone provide no database commit or recovery guarantee.
The v2 batch API adds the qualified protocol described above.
Reopen tests check visibility only. Official sync references and detailed limits
are in [feature 001](docs/feature/001-file-io.md).

## Files

- timelite.h: public declarations.
- timelite.c: public library implementation.
- file_io.h: internal file-operation contract.
- file_io.c and file_io_windows.c: POSIX and Windows implementations.
- tests/: temporary-file, deterministic fault, model and storage probe tests.
- tools/test.py, tools/inventory.json, tools/test_runner.py, tools/docker/:
  test runner, shared inventory, runner self-tests and the toolchain image.
- examples/basic.c: a small application that calls the library.
- examples/batches.c: complete v2 create/append/read/reopen application.
- [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md): coding-agent instructions.
- [docs/feature/000-init.md](docs/feature/000-init.md): bootstrap scope and results.
- [docs/feature/003-database-lifecycle.md](docs/feature/003-database-lifecycle.md): lifecycle, header, failure analysis and future WAL contract.
- [docs/feature/005-testing-suite.md](docs/feature/005-testing-suite.md): testing suite design, verification results and gaps.

## Contributions

Use a feature branch and a pull request into main. Do not push directly to main.
Each feature needs a numbered note in docs/feature describing the desired state,
actual changes, verification, and remaining work. Keep the agent instructions
and this README current as decisions change.

Licensed under the [MIT license](LICENSE).
