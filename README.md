# Timelite

A small embedded time-series database project in C99. The current version is a
library with durable sensor batch append, recovery, and sequential reading from a
separate WAL on supported local POSIX filesystems. The original v1 lifecycle API
remains available. Windows retains lifecycle and file I/O; durable batch
provisioning currently returns `ENOTSUP`. Checkpointing is deferred to feature 005.

The direction is a small C API, explicit memory ownership, no third-party
dependencies, and a narrow scope for sensor history. Target devices include x86
and ARM32; device support has not been verified yet.

## Build

On Linux or macOS, install a C99 compiler, Make, and ar, then run:

```sh
make check
```

This creates build/libtimelite.a, build/basic, build/batches, and storage test programs.
It runs the example (shown below), temporary-file tests, deterministic file-I/O, lifecycle and provisioning fault tests,
and a volatile/persisted batch storage model:

```text
Timelite 0.1.0-dev
```

Use make clean to remove build output. To select another compiler, use
make CC=clang check. Clean first when changing compiler flags or toolchains.

You can also compile the library directly into an application:

```sh
mkdir -p build
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c file_io.c examples/basic.c -o build/basic-direct
./build/basic-direct
```

On Windows x64, install Clang and the Windows SDK (including the Visual C++
build tools), then run these commands in PowerShell:

```powershell
New-Item -ItemType Directory -Force build
clang -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c file_io_windows.c examples/basic.c -o build/basic.exe
./build/basic.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. file_io_windows.c tests/file_io_test.c -o build/file_io_test.exe
./build/file_io_test.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -DTIMELITE_IO_TEST -I. file_io_windows.c tests/file_io_windows_fault_test.c -o build/file_io_fault_test.exe
./build/file_io_fault_test.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c file_io_windows.c tests/lifecycle_test.c -o build/lifecycle_test.exe
./build/lifecycle_test.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c tests/lifecycle_fault_test.c -o build/lifecycle_fault_test.exe
./build/lifecycle_fault_test.exe
```

The Makefile uses Unix shell commands. GitHub Actions has Linux, macOS, and
Windows public-library smoke checks, plus file-I/O and lifecycle tests on Linux/macOS, Windows
Server 2022 x64, and a Linux x86 32-bit job. Windows CI records OS and Clang
versions and installed SDKs. Windows runtime/SDK validation is pending; local
Windows checks so far are MinGW-w64 cross-compilation only. CI runs are separate
from validation on the target devices.

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
before writes; committed data is never overwritten or reclaimed. Feature 005
must checkpoint into main storage before reclaiming WAL capacity. This feature
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
and main-storage segments remain deferred to feature 005.

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
- tests/: temporary-file and deterministic syscall fault tests.
- examples/basic.c: a small application that calls the library.
- examples/batches.c: complete v2 create/append/read/reopen application.
- [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md): coding-agent instructions.
- [docs/feature/000-init.md](docs/feature/000-init.md): bootstrap scope and results.
- [docs/feature/003-database-lifecycle.md](docs/feature/003-database-lifecycle.md): lifecycle, header, failure analysis and future WAL contract.

## Contributions

Use a feature branch and a pull request into main. Do not push directly to main.
Each feature needs a numbered note in docs/feature describing the desired state,
actual changes, verification, and remaining work. Keep the agent instructions
and this README current as decisions change.

Licensed under the [MIT license](LICENSE).
