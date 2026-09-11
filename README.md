# Timelite

A small embedded time-series database project in C99. The current version is a
library skeleton with an internal POSIX file-I/O layer. It builds, links into
an example, and reports its version.
It does not store or query data yet.

The direction is a small C API, explicit memory ownership, no third-party
dependencies, and a narrow scope for sensor history. Target devices include x86
and ARM32; device support has not been verified yet.

## Build

On Linux or macOS, install a C99 compiler, Make, and ar, then run:

```sh
make check
```

This creates build/libtimelite.a, build/basic, and two file-I/O test programs.
It runs the example (shown below), temporary-file tests, and syscall fault tests:

```text
Timelite 0.1.0-dev
```

Use make clean to remove build output. To select another compiler, use
make CC=clang check. Clean first when changing compiler flags or toolchains.

You can also compile the library directly into an application:

```sh
mkdir -p build
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c examples/basic.c -o build/basic-direct
./build/basic-direct
```

On Windows, with Clang on PATH, run these commands in PowerShell:

```powershell
New-Item -ItemType Directory -Force build
clang -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c examples/basic.c -o build/basic.exe
./build/basic.exe
```

The Makefile uses Unix shell commands. GitHub Actions has Linux, macOS, and
Windows smoke checks, plus file-I/O tests on Linux/macOS and a Linux x86 32-bit
job. Windows compiles only the public library skeleton; its storage backend is
deferred. CI runs are separate from validation on the target devices.

## Internal file I/O

[file_io.h](file_io.h) defines the internal contract; it is deliberately absent
from the public database header. The POSIX Make build includes file_io.c in the
archive. Direct builds above exercise only the public version API.

The layer opens regular files read/write without truncating, creates exclusively
with mode 0600 (subject to umask), reads/writes at explicit offsets, obtains size,
truncates, syncs, and closes. Initialize a caller-owned `struct timelite_file` with
`TIMELITE_FILE_INIT`. Pass caller-owned buffers and byte-count outputs. Functions
return zero or a POSIX errno value. Reads stop successfully at EOF; writes finish
or return an error with the completed count. Errors do not roll back changes.
See the header for argument, ownership, range, and close-error rules.

Linux is the provisional deployment OS and macOS the local development OS.
Linux uses 64-bit file offsets, including in 32-bit builds. Exact x86/ARM32 CPU,
OS, and ABI still require confirmation. One owner must serialize calls; this
layer provides no process locking or protection from concurrent file changes.

Explicit sync uses Linux `fdatasync` or macOS `F_FULLFSYNC`, reporting failures
without a weaker fallback. It does not sync the parent directory, so creation
alone plus file sync does not promise a durable directory entry. Close is not
sync. There is no database commit, atomic-write, recovery, or power-loss guarantee.
Reopen tests check visibility only. Official sync references and detailed limits
are in [feature 001](docs/feature/001-file-io.md).

## Files

- timelite.h: public declarations.
- timelite.c: public library implementation.
- file_io.c and file_io.h: internal Linux/macOS file operations.
- tests/: temporary-file and deterministic syscall fault tests.
- examples/basic.c: a small application that calls the library.
- [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md): coding-agent instructions.
- [docs/feature/000-init.md](docs/feature/000-init.md): bootstrap scope and results.
- [research/RESEARCH.md](research/RESEARCH.md): earlier feasibility research.

## Contributions

Use a feature branch and a pull request into main. Do not push directly to main.
Each feature needs a numbered note in docs/feature describing the desired state,
actual changes, verification, and remaining work. Keep the agent instructions
and this README current as decisions change.

Licensed under the [MIT license](LICENSE).
