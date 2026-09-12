# Timelite

A small embedded time-series database project in C99. The current version is a
library skeleton with an internal POSIX and Windows file-I/O layer. It builds, links into
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

On Windows x64, install Clang and the Windows SDK (including the Visual C++
build tools), then run these commands in PowerShell:

```powershell
New-Item -ItemType Directory -Force build
clang -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c examples/basic.c -o build/basic.exe
./build/basic.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. file_io_windows.c tests/file_io_test.c -o build/file_io_test.exe
./build/file_io_test.exe
clang --target=x86_64-pc-windows-msvc -std=c99 -Wall -Wextra -Wpedantic -Werror -DTIMELITE_IO_TEST -I. file_io_windows.c tests/file_io_windows_fault_test.c -o build/file_io_fault_test.exe
./build/file_io_fault_test.exe
```

The Makefile uses Unix shell commands. GitHub Actions has Linux, macOS, and
Windows public-library smoke checks, plus file-I/O tests on Linux/macOS, Windows
Server 2022 x64, and a Linux x86 32-bit job. Windows CI records OS and Clang
versions and installed SDKs. Windows runtime/SDK validation is pending; local
Windows checks so far are MinGW-w64 cross-compilation only. CI runs are separate
from validation on the target devices.

## Internal file I/O

[file_io.h](file_io.h) defines the internal contract; it is deliberately absent
from the public database header. The POSIX Make build includes file_io.c in the
archive. Windows builds select file_io_windows.c instead; compile exactly one
backend. The public-library direct build exercises only the version API.

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
The layer does not sync the parent directory, so creation
alone plus file sync does not promise a durable directory entry. Close is not
sync. There is no database commit, atomic-write, recovery, or power-loss guarantee.
Reopen tests check visibility only. Official sync references and detailed limits
are in [feature 001](docs/feature/001-file-io.md).

## Files

- timelite.h: public declarations.
- timelite.c: public library implementation.
- file_io.h: internal file-operation contract.
- file_io.c and file_io_windows.c: POSIX and Windows implementations.
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
