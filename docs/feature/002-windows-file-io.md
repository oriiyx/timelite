# 002: Windows file I/O

Status: implemented; macOS/Linux checks and Windows cross-compilation passed.
Windows runtime and Microsoft SDK validation remain pending.
Branch: codex/002-windows-file-io, from freshly fetched origin/main 136c854.
Feature 001 is an ancestor of origin/main (verified after git fetch origin).
Staged research is outside this work and remains unchanged.

## Desired state and scope

Add synchronous Windows desktop x64 file operations with Clang and the Windows
SDK. Keep the public API unchanged, caller-owned storage, and serialized calls.
Linux remains provisional deployment and macOS local development. Exact device
CPU, OS, and ABI remain unconfirmed. No database features or locking API.

## Planned approach and verification

The initial plan was a separate Windows source with bounded UTF-8 conversion,
native synchronous calls, errno mappings, documented sharing and ownership, and
64-bit range checks. Reuse behavioral tests, add compile-time Windows fault
substitutions, and extend Windows CI while preserving existing jobs.

Run strict C99 make check locally and available Windows checks. Cover Unicode,
bad encoding, overflow, failures, ownership, EOF, short transfers, truncation,
reopen visibility, and sparse access above 4 GiB without allocating GiB. Report
unsupported sparse facilities as explicit skips. Record local, cross-compiler,
Windows runtime, CI, and device results separately. Reopen is not a durability
test. Verify staged and working research hashes remain unchanged.

## Actual implementation

- Added file_io_windows.c; Windows builds select it instead of file_io.c. The
  separate source keeps POSIX calls and fault substitutions untouched without
  adding a generic platform framework. The small validation routines follow
  the same rules in both backends. file_io.h uses a caller-owned void* Windows
  handle (NULL when closed); Windows headers stay out of the shared header.
- CreateFileW uses GENERIC_READ | GENERIC_WRITE, CREATE_NEW or OPEN_EXISTING,
  FILE_ATTRIBUTE_NORMAL, and NULL security/template arguments. No truncating,
  inheritable, overlapped, or delete-on-close flags. Create inherits directory
  ACLs; this is not a translation of POSIX mode 0600. Disk-file type and
  non-directory information are checked after opening. Use trusted file paths;
  this is not a path sandbox or a device namespace filter.
- UTF-8 paths have a deliberate limit of 259 bytes excluding NUL. A bounded
  scan precedes MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS), using a
  260-WCHAR stack array and an input length including NUL. Invalid sequences
  are rejected, including surrogate encodings. Oversized input is rejected,
  never truncated. The byte limit can be stricter than the native UTF-16 limit.
  Win32 relative/absolute path and component rules still apply; no extended-path
  prefix is added and full long-path support is out of scope.
- FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE permits compatible
  opens and allows rename/deletion while a handle remains open. Sharing checks
  are bidirectional: another open can deny access. These flags do not serialize
  writes or provide general multi-process protection. Calls require one owner
  and serialization, as before.
- SetFilePointerEx selects a checked signed 64-bit offset before synchronous
  ReadFile/WriteFile with NULL OVERLAPPED. Successful short calls continue;
  reads stop at EOF and zero-progress writes return EIO. Each transfer is capped
  at MAXDWORD before narrowing. Offset plus length must fit INT64_MAX, even on
  zero-length calls; zero-length requests do not issue native calls.
- GetFileSizeEx preserves the caller's output on failure. Shrink uses
  SetFilePointerEx and SetEndOfFile. Extension writes one zero at size minus one:
  Microsoft's documented write-beyond-EOF zero-fill preserves the shared
  zero-extension contract. SetEndOfFile alone does not promise initialized
  extension bytes. Ordinary files may allocate storage for gaps; the production
  backend does not implicitly enable sparse files.
- Added shared integration coverage with Windows-only temporary setup, Unicode
  and malformed paths, non-inheritable handles, sharing conflicts, read-only
  access failure, rename/deletion, and non-disk rejection. Existing POSIX
  symlink/FIFO checks remain. Windows temporary directories are atomically
  created under the user's trusted GetTempPathW location and inherit its ACLs;
  existing candidates are never reused or removed. Cleanup uses Unicode APIs
  and only owned paths.
- The Windows large-offset test requires successful FSCTL_SET_SPARSE before
  writing above 4 GiB. Unsupported function/filesystem cases print an explicit
  skip; unexpected setup failures fail the test. Only tiny writes and extension
  follow, never a multi-GiB zero-buffer write.
- Added Windows fault tests for partial progress, EOF variants, zero writes,
  seek/size/truncate/sync/open/validation/close failures, mappings, and ownership.
  Native-call substitution is compile-time only. A three-byte test transfer cap
  exercises chunking with small buffers; production uses MAXDWORD. No test
  globals are linked into the backend.
- Windows CI now targets windows-2022 x64, keeps the public-library example,
  and builds/runs both backend test programs with Clang and Windows SDK headers.
  It records OS, compiler version, and installed SDKs. Linux/macOS and Linux
  x86 32-bit jobs remain. README, AGENTS.md, and CLAUDE.md reflect this scope.

## Error and ownership contract

Return values remain zero or errno constants; Windows does not expose a new
native-error object or promise to preserve GetLastError. The mapping is lossy:

| Native error (ERROR_ prefix omitted) | Return |
| --- | --- |
| FILE_NOT_FOUND, PATH_NOT_FOUND | ENOENT |
| FILE_EXISTS, ALREADY_EXISTS | EEXIST |
| ACCESS_DENIED, SHARING_VIOLATION, LOCK_VIOLATION, WRITE_PROTECT | EACCES |
| INVALID_HANDLE | EBADF |
| INVALID_PARAMETER, INVALID_NAME, BAD_PATHNAME, NEGATIVE_SEEK | EINVAL |
| ARITHMETIC_OVERFLOW | EOVERFLOW |
| FILE_TOO_LARGE | EFBIG |
| DISK_FULL, HANDLE_DISK_FULL | ENOSPC |
| NOT_ENOUGH_MEMORY, OUTOFMEMORY | ENOMEM |
| TOO_MANY_OPEN_FILES | EMFILE |
| FILENAME_EXCED_RANGE, INSUFFICIENT_BUFFER (path conversion) | ENAMETOOLONG |
| NO_UNICODE_TRANSLATION | EILSEQ |
| NOT_SUPPORTED, INVALID_FUNCTION | ENOTSUP |
| Other errors, including cancellation and device failures | EIO |

HANDLE_EOF during read is successful EOF. API validation uses EINVAL, EBADF,
EOVERFLOW, and ENAMETOOLONG directly. POSIX paths remain native byte strings;
UTF-8 validation is Windows-only. Windows sharing/write-protection errors lose
some native detail in EACCES. No POSIX EINTR retries are applied to Windows.

Open/create require a closed handle. Failure leaves that caller handle closed;
post-open validation attempts CloseHandle once and preserves the initial error
if cleanup also fails. A failed create may leave a new path. Failed reads,
writes, size, truncate, or sync retain caller ownership; transfer counts report
successful prior calls, and failed operations can leave changed contents or
file position. Errors do not roll back changes.

Close consumes the caller's handle, setting it to NULL, and invokes CloseHandle
once. A subsequent close returns EBADF without another native call. This is an
explicit library ownership policy, not an assumption of POSIX close semantics.
Microsoft describes success/failure but does not give a universal resource-state
guarantee for every failure. A failed close (including validation cleanup) can
therefore leave uncertain OS resource state; the library neither retries nor
promises leak-free recovery. A new open may reuse the closed caller struct.

## Sync contract and official references

Microsoft's [FlushFileBuffers documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
describes flushing buffered information for an open file to the device and
requires write access. The backend calls it directly and reports failures,
including unsupported requests, without a weaker fallback. A successful call
is a file flush request, not proof that hardware honors it under power loss.
No volume or parent-directory flush is performed. Creation, renaming, or
unlinking followed by file sync is not promised to make directory changes
durable. No equivalence with Linux fdatasync or macOS F_FULLFSYNC is claimed.

[CloseHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle)
is handle release, not this layer's explicit sync operation. Close and reopen
checks demonstrate content visibility only. There is no database commit,
atomic-write, recovery, or unconditional power-loss contract.

Other official references checked during implementation:

- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew): creation, sharing, and synchronous handles.
- [MultiByteToWideChar](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar): strict UTF-8 conversion and buffer lengths.
- [SetFilePointerEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfilepointerex): 64-bit positioning and write-beyond-EOF zero initialization.
- [SetEndOfFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setendoffile): undefined extension contents when used alone.
- [ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile) and [WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile): synchronous transfer counts and EOF handling.
- [FSCTL_SET_SPARSE](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_sparse): explicit sparse-file setup for tests.

## Verification results

Local macOS ARM64, Darwin 25.6.0, Apple Clang 21.0.0:

- make check: passed, warning-free strict C99; public example, shared integration
  including sparse >4 GiB, and original POSIX fault tests.
- git diff --check: passed.

Local disposable Linux AArch64 container, gcc:14, GCC 14.4.0:

- Read-only source copied into temporary container storage; make clean followed
  by make check passed, including sparse >4 GiB and both test programs.
- This is host/container validation, not the target device or 32-bit ABI.

Windows x64 cross-compilation in a disposable Linux AArch64 container:

- MinGW-w64 GCC 14.2.0 (reports 14-win32), MinGW-w64 12.0.0 headers.
- x86_64-w64-mingw32-gcc with -std=c99 -Wall -Wextra -Wpedantic -Werror:
  backend plus shared integration tests passed; backend plus Windows fault tests
  with -DTIMELITE_IO_TEST passed; public library plus example passed.
- Executables were compiled and linked only, not run. This does not validate
  Microsoft SDK/Clang compatibility, Windows behavior, or durability.

Windows runtime: not run; no Windows version/toolchain has runtime results yet.
CI: configured for Windows Server 2022 x64, not published or run for this change.
Linux x86 32-bit CI: preserved, not run locally in this feature.
Target devices: untested; exact CPU/OS/ABI remain unconfirmed.

Research worktree SHA-256 remains
`ae906c67f4cf80461ed64337c2b01c8d5fd257b9005e56c87aecdd874c2cbae4`.
Staged-diff SHA-256 remains
`ecf26fdc09135ec1ca3424691bee174cebded9cfa1bcef1bc3520b7bd752646c`.
No feature files were staged; no commit, push, or merge was performed.

## Remaining limits

Windows runtime and actual Clang/SDK results must be recorded after CI executes.
Other Windows versions, ARM32, and physical target devices remain unvalidated.
No long-path support, process locking, directory durability, power-cut tests,
recovery, or database behavior was added. Use trusted regular-file paths and a
trusted private temporary directory for tests. Reopening is not durability proof.
