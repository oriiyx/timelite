# 001: Basic file I/O

Status: implemented; local macOS and Linux container checks passed.
Branch: codex/001-file-io, from origin/main b9136d1 (bootstrap PR #1 merged).
Existing staged research is outside this feature and must remain unchanged.

## Desired state and scope

A small internal, allocation-free file layer, separate from timelite.h and the
future database API. Linux is the provisional deployment OS; macOS is the local
development OS. Exact x86/ARM32 CPU, OS, and ABI remain unconfirmed. Windows keeps
its library smoke build; its storage backend is deferred.

Only synchronous regular-file operations are included: exclusive create,
non-truncating open, offset read/write, size, truncate, explicit sync, and close.
No database format, recovery, locking, transactions, mmap, or background work.
One owner and serialized calls are required; there is no multi-process protection.

## Implementation approach (completed)

- Add internal file_io.h and file_io.c using POSIX calls and caller-owned handles
  and buffers. Return zero or a POSIX errno value directly; no hidden error state.
- Use pread/pwrite loops with EINTR retry, short-transfer handling, explicit byte
  counts, and checked signed 64-bit offsets. Enable large-file Linux interfaces
  before system headers and reject incompatible off_t at compile time.
- Use fdatasync on Linux and fcntl(F_FULLFSYNC) on macOS. Report errors, including
  unsupported sync; do not silently weaken the operation.
- Consume the handle on close even on error; never retry close because a released
  descriptor could be reused. An interrupted close can have platform-dependent
  resource state, so successful cleanup cannot be promised after that error.
- Use a compile-time syscall substitution seam in a separate test executable;
  no mutable test hooks in the production library.

## Sync contract and sources

Linux fdatasync writes file data and metadata needed to retrieve it (including
size). A separate parent-directory fsync is needed for directory-entry durability;
this layer does not provide it. See the upstream
[Linux man-pages fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html).

Apple documents that fsync alone may leave data in drive caches. F_FULLFSYNC also
requests flushing those caches; unsupported operations and device errors are
reported. Hardware can fail or ignore flush requests. See Apple's
[fsync manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)
and [fcntl manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html).
These official references were checked during implementation. Sync is a file-level
request, not a database commit, atomic write, recovery protocol, or unconditional
power-loss guarantee. New directory entries are not made durable by this layer.

## Verification plan

Run strict C99 make check locally: library example, temporary-file integration
tests, and deterministic syscall fault tests. Cover exclusive create, existing
open, offset reads/writes, EOF, size, shrink/extend, reopening, invalid arguments,
errors, interrupted calls, short transfers, zero writes, and sparse offsets above
4 GiB. Use a private mkdtemp directory and remove only known test paths.
Extend Linux/macOS CI and add a Linux 32-bit large-offset run; preserve Windows.
Report local results separately from unrun CI and target-device checks. Reopening
checks visibility only, never power-loss durability.

## Actual changes and results

- Added file_io.c/file_io.h as an internal module in the POSIX archive; the
  public version API and Windows direct build are unchanged.
- Added regular-file validation and close-on-exec descriptors. Nonblocking open
  prevents waiting on FIFOs before rejecting them; it has no effect on regular
  file transfers. Existing-file open follows symlinks, so paths must be trusted.
- Added integration tests and a separate executable with compile-time syscall
  substitutions. Production has no test state or heap allocation.
- Extended make check and Linux/macOS CI; added a Linux x86 32-bit job using
  gcc-multilib. Updated README and both agent instruction files for this scope.
- Open/create preserve the first failure if descriptor cleanup also fails. A
  failed create after opening can leave an empty path. Close errors consume the
  handle; EINTR can leave platform-dependent resource state. These are documented
  limits, not hidden retries or rollback promises.

Local macOS ARM64, Apple Clang 21.0.0:

- make check: passed with default strict C99 warnings, including sparse >4 GiB,
  reopen visibility, EOF, truncation, invalid inputs, and deterministic failures.
- make clean followed by make check with AddressSanitizer/UndefinedBehaviorSanitizer
  and -O1: passed. Subsequent test-only additions passed the default check.
- Direct public-library compilation and example using the README command: passed.
- Initial test compilation found macOS hiding mkdtemp under strict POSIX feature
  selection; enabling Darwin declarations in the test fixed it.
- git diff --check: passed. Research worktree SHA-256 and staged-diff SHA-256
  match their pre-implementation values; nothing has been staged by this feature.

Linux AArch64 local disposable container, GCC 14.4.0 (gcc:14): make check passed
with strict C99 warnings, both file-I/O test programs, and sparse >4 GiB. Source
was mounted read-only and copied to temporary container storage. This checks the
Linux backend on a virtualized host, not a target device or the 32-bit ABI.
GitHub Actions: configured; results pending publication and CI execution.
Target devices: untested; exact CPU/OS/ABI remain unconfirmed.

## Remaining limits

Windows storage is deferred; the Windows smoke job has not run locally. No ARM32
or physical x86 device validation has occurred. Sparse tests explicitly report a
skip only for filesystem size/support limits. No power-cut tests were performed;
reopen is not evidence of crash or power-loss durability. No directory sync,
locking, atomic-write, database recovery, or transaction behavior is provided.
The user authorized committing and pushing feature 001 for a pull request.
Merge remains with the user. Publication and CI results belong on the pull
request; the existing research edits are excluded from the feature commit.
