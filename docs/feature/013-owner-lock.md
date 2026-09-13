# 013: Single owner of a batch pair

Status: implemented; make check and the local suite passed. Clean current main checkout, as explicitly
requested; one agent, no Git mutations.

## Decisions and scope

Acquire an exclusive nonblocking kernel lock on the database immediately after
open/create, before header reads or writes. POSIX uses flock(LOCK_EX | LOCK_NB)
with no retries; contention maps to EBUSY, EINTR and other errno propagate.
Windows uses LockFileEx over the whole range with exclusive/immediate flags;
ERROR_LOCK_VIOLATION maps to EBUSY only here. Other operations retain feature
002's EACCES mapping. Close releases ownership, including poisoned handles;
failed close consumes the handle but leaves native release uncertain.

Reject lock files because crashes leave stale files. Reject fcntl record locks
because they are per process and closing unrelated descriptors drops them.
Reject a new negative error because positive errno is already supported. Lock
only the database: the WAL is reached through pair open, so locking both adds
nothing under the trusted-path contract. Reject v1 locking to preserve its
existing lifecycle contract. No format or public API change, allocation,
threads, blocking wait, fallback or automatic policy.

The lock is advisory: external code can modify the files. It does not permit
sharing a pair across processes or serialize calls within the owning handle.
Windows provisioning remains unsupported. verify remains read-only and lock-free
and can observe a moving target; status refuses a held pair with EBUSY/exit 2.

## Planned boundaries

| Boundary | Effect and cleanup |
| --- | --- |
| Argument rejection | Existing caller handle unchanged; no acquisition |
| Database open fails | No database owned; backend cleans validation failures |
| Creation precheck finds WAL | Close precheck WAL once; no database created |
| Identity fails | Neither file acquired |
| Database lock fails | Close database once; no header read/write or WAL creation/open |
| WAL open/create fails after lock | Close database once, releasing lock |
| Header/provision/recovery fails | Close each acquired resource once, WAL then database |
| Success, including later poison | Both resources and lock held until close |
| Cleanup close fails | Preserve first error; native resource/lock release uncertain |

Creation probes the WAL name before creating the database; this probe reads no
body. Existing-open and race-winner paths lock before any header read.

## Planned verification

Extend existing backend behavior/fault, batch, model and inspect tests; no new
inventory entries. Run make check, then python3 tools/test.py run local.
Container preflight deferred; CI, Windows runtime and devices unrun.

## Actual changes and ordering

Added timelite_file_lock to both native backends and the batch open paths;
inspect names EBUSY. Updated internal/public contracts, README and agent summaries.
All tests reuse existing inventory entries. The v1 fault backend additionally
supplies the new symbol and asserts it is never called by v1.

Existing open: validate arguments/closed handle/scratch, open database, lock,
validate database header, open WAL, validate both headers/identity, database size,
manifests and segments, WAL size and frames, orphan checks, provision database
then WAL, finish retention if needed, truncate/sync recoverable WAL suffix,
then publish both resources to the caller handle.

Create: validate arguments, probe absence of WAL, obtain identity, exclusively
create database, lock, exclusively create WAL, write/sync database and WAL
headers, then common validation/provisioning/recovery above. Open-or-create's
EEXIST race performs one database open, then follows the existing-open lock path.
Windows identity still returns ENOTSUP before creation; otherwise-valid existing
pairs acquire the lock and later fail provisioning with ENOTSUP.

Every error closes only acquired resources, once, WAL before database, preserving
the original error. In particular a lock failure has ONE acquired resource,
the database; the WAL remains unopened (not closed spuriously). This resolves the
request's "both resources closed once" test wording in favor of its required
lock-before-WAL ordering. Both model resources end unowned. Argument rejection
leaves caller storage unchanged. All other failed opens leave a closed reusable
handle. A native close failure always leaves actual release uncertain.

Coverage added: fresh locks, same-process contention, unrelated file, release and
reopen; POSIX fork with pipe synchronization (child drops inherited descriptor,
sees EBUSY, then acquires after parent closes); syscall fault propagation without
retry; model EBUSY/EIO on existing/create/race paths with zero reads, writes,
truncates or provisions and first error preserved over close failure; real pair
byte/size preservation and original-owner append after refusal; inspect EBUSY
and byte-identical verify while a poisoned owner retains the lock.

## Findings and remaining limits

No additional library defect found. Windows creation's existing identity ENOTSUP
precedes file creation, so only existing-pair open can reach the lock there.
Feature 006 wording and the documentation defects listed in feature 012 are
untouched. The old blanket "no locks" statements in current batch/backend
contracts were updated for this feature.

Advisory ownership does not stop bypassing code, path replacement or aliasing
that violates the trusted pair contract. Fork inherits the POSIX open description;
its lock lasts until all inherited copies close, so applications must not retain
or use inherited batch handles. Network filesystems (NFS/SMB), unusual mount
options and other filesystems can have different/unreliable flock semantics;
no such storage is validated here, and batch provisioning retains its local
filesystem allowlist. This is no multi-process sharing or thread-safety promise.
Process death without inherited copies releases kernel ownership; no physical
power-loss guarantee is added. Windows runtime, CI, devices, Linux/32-bit and
container cross-builds remain unrun for this feature; preflight is deferred.

## Verification actually run

Local host reported by runner: macOS 26.6.2 arm64, Apple toolchain (`cc`),
Python 3.9.6. No CI, Windows runtime, device or container results are claimed.

- First make check: 11 passes, one optional Windows skip, inspect build failed
  because the new test case was inserted in the wrong fixture helper. Corrected
  the test placement; no library failure was involved.
- Second make check: 12 passes, one optional Windows skip. Then strengthened
  coverage for poisoned-owner lock lifetime and duplicate-close detection.
- Final make check: PASS 12, SKIP 1, 10.3 seconds;
  build/test-runs/run-20260913-225548-91bx43nz.
- python3 tools/test.py run local: PASS 25, SKIP 2 (Windows-only fault test in
  native and sanitize), 36.1 seconds; report is not stale:
  build/test-runs/run-20260913-225608-t7tk45ft/report.json.
  Native, AddressSanitizer/UndefinedBehaviorSanitizer and runner self-tests passed.
- git diff --check passed.

Only this feature note's result record changed after the final run; production
and test source are unchanged. Testing stopped. No Git mutations were performed.
