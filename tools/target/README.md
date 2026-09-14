# Target evidence protocol

Hardware execution is deferred. This tooling does not identify or control a power
switch. It adds no library fault hooks. Use only dedicated disposable storage.

## Confirm the target before preparing a toolchain

Record device model, CPU ISA (including required extensions), OS/kernel,
endianness, ABI and float calling convention, libc/version, compiler/linker and
sysroot. Record drive model/firmware/interface, filesystem and mount options,
partition, cache/flush configuration and power domain. Identify the disposable
paths, login, and independent controller whose evidence storage remains powered.
Obtain explicit authorization naming device, storage scope and physical power
action before any cut. A relay cutting only CPU power while storage remains on
is a different experiment; describe exactly which components lose power.

Install the vendor SDK only after matching those facts. Retain its immutable
archive/image digest and sysroot identity. Do not silently replace it with a
convenient ARM compiler. Create a JSON configuration outside the checkout:

```json
{
  "device": "confirmed exact model",
  "isa": "confirmed ISA and extensions",
  "os": "Linux distribution and version",
  "kernel": "exact release",
  "abi": "confirmed ABI, endianness and calling convention",
  "float_abi": "hard",
  "libc": "confirmed implementation/version",
  "sdk_identity": "immutable vendor SDK and sysroot SHA256 digests",
  "triple": "exact compiler -dumpmachine output",
  "cc": ["/absolute/sdk/bin/target-gcc"],
  "ar": ["/absolute/sdk/bin/target-ar"],
  "flags": ["--sysroot=/absolute/sdk/sysroot"],
  "link": [],
  "libs": []
}
```

These are placeholders, not an SDK recommendation. Set float_abi to `soft` for
the base ARM calling convention (including softfp); put exact -march/-mcpu,
-mfloat-abi, -mfpu and endianness options in flags as required by the target.
Then run:

```sh
TIMELITE_ARM32_CONFIG=/absolute/confirmed-target.json python3 tools/test.py run arm32-cross
```

The profile reuses all POSIX inventory entries, including static-library
examples, fake-backend models, native backend tests, workload and standalone
inspect. It never executes any generated target binary. Host CC/CFLAGS are not
inherited. Reports embed the supplied configuration, compiler identity, macros,
triple, ABI description, complete compile/link commands and a 64-bit off_t
compile/link assertion. Missing configuration/tools and mismatched ARM32 Linux
macros, pointer size, triple or float calling convention block compilation.
ISA/libc/sysroot compatibility still requires review of SDK provenance and the
retained macro log; configuration text alone does not prove compatibility.
`prepare` continues to prepare existing Docker profiles only. ARM32 is excluded
from `local` and `preflight`. No emulated run is device evidence.

## Real-device baseline (outstanding)

Disable automatic workload startup first. Install Make, Python 3, the matching
native compiler/archiver and sanitizer runtime. Copy the reviewed source tree
and preserve its digest (the runner snapshots it). Record commands, outputs and
exit codes on the independent controller, including:

```sh
uname -a
cat /proc/cpuinfo
cat /etc/os-release
cc --version
cc -dumpmachine
findmnt -T /absolute/disposable-test-root -o SOURCE,TARGET,FSTYPE,OPTIONS
lsblk -o NAME,MODEL,SERIAL,REV,TRAN,FSTYPE,MOUNTPOINTS
python3 tools/test.py run native --test storage_probe --test-root /absolute/disposable-test-root
python3 tools/test.py run local --test-root /absolute/disposable-test-root
```

Retain /proc/self/mountinfo, compiler macro output, libc/SDK identity, and
read-only device-specific cache configuration. Inspect `findmnt -T` for each
reported storage root, not merely the source checkout; compare device numbers
from `stat` if necessary. The runner uses private children of --test-root and
passes it as the self-tests' temporary root as well. Archive report.json,
junit.xml, snapshots and all logs off-device before any cuts. Provisioning
allowlist success is a prerequisite, not a certification. Missing tools or
sanitizer support are BLOCKED/NOT RUN in the evidence ledger; preserve runner
failures, do not weaken local or substitute container storage. Investigate test
exit 78 (required sparse groups not run). No unsupported-storage fallback.

## Controller transport and oracle

Build `target_workload` and `timelite-inspect` through the native inventory on
the device (or use binaries from its verified cross-build). Their paths appear
in the runner output directory. The Python controller requires POSIX Python 3
and runs on the separate always-powered machine. Controller fsync (Linux) or
F_FULLFSYNC (macOS) and directory fsync must succeed on its evidence storage.
Ensure its filesystem/device honors those flushes. No new dependencies.

A fresh trial starts with exclusive `create`, never open-or-create. Send a
bounded plan of at most 256 commands over a dedicated SSH connection:

```sh
python3 tools/target/evidence.py collect --plan tools/target/plan.txt \
  --journal /controller/campaign/trial-001.jsonl -- \
  ssh -T DEVICE /absolute/target_workload create /test/trial-001/db /test/trial-001/wal
```

Create and durably provision the test directory ancestry before starting.
SSH must have no stdout banners, forced interactive shell or PTY. Use trusted
fixed paths without shell metacharacters: SSH remote arguments are interpreted
by the remote shell. Workload stdout is protocol only; stderr is retained next
to the journal. A second operator can trigger the separately authorized physical
power removal while collect waits; the script never cuts power itself.

A commands append one record (series 14, timestamp=value=ID); C commands explicitly
checkpoint (argument 1 is unused); R commands expire before their cutoff.
IDs are 1..1000000 and strictly increase. Each acknowledged checkpoint groups
pending batches into a segment. The supplied plan exercises pending WAL,
multiple segments, retention with pending WAL, complete expiration and continued
append. Use a prefix ending at the operation being studied to avoid uncertainty
about which request follows it. Include empty-WAL and pending-WAL checkpoint
cases and retention retaining some versus no segments in the trial matrix.

Ordering is strict: durable intent -> send request -> successful library call
-> device flushed stdout response -> controller validates response -> durable
ack journal -> print acknowledgement/send next request. Only the durable journal
counts as acknowledgement. A timeout, lost response, broken pipe or device death
leaves one possible operation in flight. Do not retry blindly. stdout flush is
transport delivery, not durable controller logging. Controller journal corruption
fails closed and requires investigation; never infer acknowledgements from it.

The oracle starts from an empty pair, tracks returned sequence numbers, segment
groups, pending WAL and historical timestamp floor. An interrupted append permits
its full record present or absent. Interrupted checkpoint permits old pending WAL
or installed segment state with identical logical records. Interrupted retention
permits exactly the whole old set or whole new set; PREPARE normally recovers old,
TAIL/CLEANUP new. It never permits removal of WAL, retained segments or a partial
eligible segment. Acknowledged retention requires the new set. A coherently
recovered pair that drops an acknowledged required record fails the oracle.
Corruption/fail-closed recovery is a failed recovery trial, even if inspection
correctly detects it; do not hide it as a successful durability result.

## Restart, preserve, inspect, compare

After power returns, prevent any automatic recovering open. Confirm the workload
has exited and no owner remains. Normal inspection requires graceful close;
a crash requires confirmed process death. Never bypass Feature 013's lock.
First preserve both files and logs in a new trial directory on supported device
storage; the command requires the operator's explicit quiescent assertion:

```sh
python3 tools/target/evidence.py preserve /test/trial-001/db /test/trial-001/wal \
  /test/evidence/trial-001 --inspect /absolute/timelite-inspect \
  --log /absolute/device-log --quiescent
```

This refuses an existing destination, copies originals and logs, records pair
SHA256 hashes and flushes originals before recovery. It runs read-only verify
on the original copy first, retains output/exit code, then status --dump on a
working copy. Originals are never opened through the library. It does not take
ownership on the user's behalf or protect against an external writer: establishing
quiescence is mandatory. If preservation fails, stop and retain partial evidence.
Original source paths are untouched. Copy the entire trial directory, relevant
boot/kernel logs and journal to the controller; verify hashes there. Keep working
copies on supported filesystems. Then on the controller:

```sh
python3 tools/target/evidence.py compare /controller/campaign/trial-001.jsonl \
  /controller/campaign/trial-001/status.txt
```

Require status exit 0 as recorded in inspection.json as well as comparison PASS.
Both old/new outcomes matching an interrupted checkpoint remain ambiguous about
its installation, even though preservation can pass. The JSON marks an
interrupted operation and whether sequence high-water has been checked.

To test high-water preservation, after the original comparison append a fresh ID
larger than every intended ID to the **working copy** using workload `open`, then
close and run status --dump again. Preserve the exact command, response, exit
status and resulting dump. Compare with `--continuation ID` against the original
journal. This requires sequence = previous high-water + 1 even when retention
removed every record. The original comparison must pass first to check the old
timestamp floor; continuation alone changes that floor. Never resume before this
reconciliation. Repeat restart/recovery/append on that working pair for recovery
checks, preserving a new copy at each restart. Start each new physical trial from
a fresh pair and journal: this oracle deliberately does not merge trial histories.

## Trial ledger and limits

Maintain a controller ledger with unique ID, source/SDK/binary hashes, baseline
report, device/storage configuration, exact plan and journal, observed last
intent/ack, power mechanism/operator authorization, controller monotonic and UTC
cut timestamps, requested delay, measured power-off interval, boot completion,
kernel/storage logs, artifact paths, verify/status exits, comparison and
continuation result. Record counts by requested phase and observed phase:
pending WAL append, checkpoint installation/reclaim, partial/full retention,
and restart/recovery. A timed delay does not identify an internal write/sync
boundary. Label phases as observed intent/ack windows; inspect manifests can
provide additional on-disk observations but not exact power timing.

Retain every failure and ambiguous trial. Report totals including unattempted,
blocked and ambiguous trials; never delete or rerun over them. Process termination,
reboot, mock journals and volatile model tests belong in separate totals from
physical cuts. A finite campaign supports only the tested device/OS/ABI/storage,
mount/cache configuration and timing distribution, subject to the documented
flush and previously-synced-byte assumptions. Coherence alone proves no durable
acknowledgement preservation. Host tests and prepared scripts do not complete
the device evidence objective.
