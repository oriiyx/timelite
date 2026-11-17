# 015: Floating-point sensor values

Status: implemented; final host and cross checks pending below.
Branch: codex/015-floating-point-values. Initial main tree and index were clean.
No commit or push authorized or performed.

## Desired state and approved design

Exact int64 and finite IEEE 754 binary64 readings coexist, including within one
batch. Types and values survive sequential/window reads, checkpoint, retention
and recovery. Timestamps remain uint64 microseconds and series remain uint32.
Caller-owned memory, serialized ownership and the qualified durability contract
are unchanged. No dependencies, allocation, background work or migration.

The user approved the typed-value proposal before implementation, then clarified
there are no deployed databases and legacy complexity is unnecessary. Accordingly,
new pairs use v3 by default through the existing open API, without a separate
create API or opt-in format. Existing integer structs/signatures are retained.
Existing v2 pairs remain readable and integer-writable without an open-time
format upgrade. A single scratch constant is now 1600 for all batch operations.
These decisions supersede the initial proposal's separate typed creation API
and separate scratch constant. Historical feature notes describe their versions;
this note defines the current extension.

## Public API and memory

Add enum timelite_value_type (INT64=0, FLOAT64=1), struct timelite_value containing
that tag and a named `data` union (`integer`, `floating`), and
struct timelite_typed_record (`series`, `timestamp_us`, `value`). Add
struct timelite_typed_aggregate (`record_count`, typed `minimum_value`, typed
`maximum_value`). Public names are prefixed with timelite_/TIMELITE_.

Add these timelite_batches_ functions, using existing argument conventions:

- append_typed: typed records, count, scratch, scratch size, sequence output.
- next_typed: typed output records, capacity, count/sequence outputs, scratch.
- next_range_typed: the same plus the existing range filter.
- aggregate_range_typed: existing range, typed result and scratch.

Reuse init/open/seek/rewind/checkpoint/expire_before/close and ownership. Status
adds `format_version` (2 or 3). `timelite_float64_supported()` returns 1 or 0.
Keep struct timelite_record, struct timelite_aggregate and their APIs unchanged.
Integer append works on both versions. Typed all-integer append works on v2;
floating append to v2 returns UNSUPPORTED_VERSION before effects. No conversion,
scaling or quantization occurs. Existing integer readers/aggregation return
TIMELITE_VALUE_TYPE_MISMATCH (-10) if any selected value is floating, including
integral-valued doubles. Unselected float records do not cause a type error.
Errors leave outputs and cursor unchanged; aggregation always preserves cursor.

TIMELITE_BATCH_SCRATCH increases from 1344 to 1600 bytes. Applications using the
macro remain source-compatible after rebuilding; hardcoded 1344-byte buffers
must grow. Short buffers are rejected before effects. No stable ABI is promised;
rebuild callers, as the private handle and status structures changed. Batch count
remains 1..64 and record output capacity is in records. Shared append uses at
most 64 temporary integer records; typed aggregation uses at most 64 typed
records and a borrowed cursor. These fixed local arrays add bounded stack use
(up to 1536 and 2048 bytes respectively with the host ABI, plus scalar/handle
locals). Caller scratch holds the whole encoded frame. No pointers are retained.

## Persistent bytes and compatibility

New database/WAL pairs have header version 3, encoded little endian at offset
8 in both 32-byte headers. Magic, 128-bit identity, header CRC, manifests,
segment layouts, capacities and persistence ordering remain unchanged. Version
and identity must match before provisioning/recovery. Existing binaries whose
pair_header accepts only version 2 return UNSUPPORTED_VERSION on version 3
before recovery or writes, even on empty or completely expired databases.
This follows directly from the previous header gate; no old-binary runtime
execution is claimed. Existing pair headers are never rewritten on open.

Integer append retains the original compact frame: TLBATCH! header marker zero
at offset 24, 20-byte records, explicit modulo-2^64 signed payload encoding.
Typed append on v3 uses marker 15 and 24-byte records, even if all are integer.
Both frame forms coexist within segments and the WAL. The shared parser accepts
both known encodings; marker interpretation is independent of pair version.
The library never writes typed frames to v2. No conversion or rewrite of old
frames is needed. Moving history between pairs is outside this feature.

| Typed record offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | series, little endian |
| 4 | 8 | timestamp_us, little endian |
| 12 | 8 | int64 modulo-2^64 or binary64 sign/exponent/fraction payload, little endian |
| 20 | 4 | tag: 0 int64, 1 binary64, little endian |

The frame header remains 32 bytes; body length is 32 + stride * count. The
32-byte TLCOMMIT trailer retains its sequence, count, body length, body CRC
and trailer CRC. Maximum compact/typed frame sizes are 1344/1600. Explicit byte
encoding is used, never native structs or native-endian doubles. Integer decode
avoids out-of-range unsigned-to-signed casts. Validated frame bytes are copied
unchanged during checkpoint and retention, including recovery copies.

Unknown markers/tags and nonfinite committed payloads are invalid data. A
complete corrupt frame is never discarded as an incomplete suffix. Only a
valid header declaring a frame past EOF licenses the existing incomplete-suffix
recovery; short headers remain ambiguous and fail closed. Installed frame
validation remains lazy (during reads/copies); this feature does not add a full
installed-payload scan on open.

## Floating representation and special values

Binary64 preserves ordinary C double readings without binary32 quantization.
All finite bit patterns are supported, including both signed zeros, smallest
subnormal, largest subnormal, minimum normal and finite extremes. NaN (including
signaling encodings) and either infinity return EINVAL before append effects.
Committed nonfinite payloads are INVALID_DATABASE and are checked as bits,
without evaluating untrusted floating data. Preserve every accepted payload bit.

Support requires 8-bit bytes (existing library requirement), sizeof(double)==8,
FLT_RADIX==2, DBL_MANT_DIG==53, DBL_MIN_EXP==-1021 and DBL_MAX_EXP==1024.
Memcpy-based probes require double and uint64 to agree on the standard binary64
bit layout for 1.0, -0.0, -2.5, the smallest subnormal and largest finite value.
Ordinary matching little/big endian layouts are admissible; mixed word order or
unrecognized representations are rejected. Numeric-limit preprocessor gates
exclude binary64 literals on incompatible implementations. No union punning,
strict-aliasing violation, native-endian disk write or long-double assumption.

`__FAST_MATH__`, positive `__FINITE_MATH_ONLY__`, or explicit
TIMELITE_DISABLE_FLOAT64 disable floating support. Build without floating
transformations that discard signed zero or finite precision. Applications
receive the value they supplied; the library cannot restore bits already lost
by upstream arithmetic/compiler choices. Checks qualify a representation, not
an arbitrary compiler or device. The exact target CPU/OS/ABI still needs evidence.

Unsupported hosts retain integer operations, pair open/recovery, checkpoint,
retention and bit-level inspect verify. Appending floats or returning selected
floats through typed readers returns ENOTSUP; integer-only typed calls still
work. This is simpler than rejecting all v3 access on such hosts and avoids
making integer storage depend on native floating support. No fallback to binary32.

## Aggregation

Count every selected record. Integer-only extrema remain exact int64;
float-only extrema retain their input bits. Mixed extrema are typed copies of
actual selected records. Compare mixed magnitudes using binary64 exponent and
significand, bounded unsigned shifts, integer part and a fraction flag. Never
convert an integer to double or perform an unchecked float-to-int cast.
Comparisons cover the whole int64 and finite binary64 domains.

Numerical ties retain the first record in stored sequence/record order for both
extrema, including INT64_MIN versus -2^63, int 0, +0.0 and -0.0. Empty results
have count zero and integer-zero extrema. Filtering and unit compatibility
remain application responsibilities. Errors preserve the complete result.

## Tooling and actual changes

Core changes are in timelite.c/h. Inspect mirrors frame validation, recognizes
v2/v3 headers, rejects mismatched versions, reports status `format_version`
and corresponding prediction, and dumps `type=int64` or `type=float64`.
Integers print exact decimal; doubles print 17 significant digits and their
16-digit hexadecimal payload, making signed zero and exact bits explicit.
`verify` remains read-only and `status` retains its recovering-open behavior.

examples/typed.c demonstrates an integer beyond 2^53 beside a fractional float,
checkpoint and exact mixed aggregation. Make and inventory build it. Existing
integer examples remain unchanged. README and agent summaries are updated.

Native/model tests share tests/typed_cases.h. Coverage includes 64-record mixed
batches, signed zero, finite extremes, subnormal/normal boundaries, negative and
fractional values, invalid tags and all nonfinite categories, filtered reads,
seek, insufficient scratch/output, output/cursor atomicity, empty aggregation,
exact precision boundaries in both input orders, reopen/checkpoint/retention.
The model exercises typed append/checkpoint/retention writes and syncs before
and after effects, unsynced bytes surviving/disappearing, truncation failures,
interrupted recovery writes/syncs/truncations, all 1568 body prefixes and all
32 commit prefixes, valid-CRC invalid payloads, and v2 integer compatibility.
Inspect compares read-only verification predictions with recovering status for
typed pairs and every interrupted typed retention operation; corrupt nonfinite
payloads fail closed. A separate inventory configuration compiles the model
with TIMELITE_DISABLE_FLOAT64, reruns integer regression, and checks harmless
ENOTSUP on floating append/read while bit-preserving checkpoint stays usable.

## Verification record

Initial make check exposed only fixtures expecting version 2 and old dump text;
updated them. A typed append fault expectation omitted the case where an
unsynced complete commit survives; corrected the oracle. These were test
expectation failures, not production fixes.

- Intermediate make check: PASS 16, SKIP 1 optional Windows-only test;
  build/test-runs/run-20260914-101047-n9pq_c7l. Additional recovery cases and
  documentation followed, so this is not the final-source result.
- Final targeted model after adding recovery-truncate/commit-prefix coverage:
  PASS, build/test-runs/run-20260914-101234-ovrusn_h.
- Toolchain doctor initially hit the sandbox's Docker socket restriction.
  Approved execution outside the sandbox found Docker and the prepared pinned
  amd64 image: build/test-runs/doctor-20260914-100811-6qwclj2g.
- Final local/preflight results: pending.

## Remaining limits

No automatic migration, binary32, nonfinite storage, sum/mean or schema policy.
Windows durable batch provisioning remains ENOTSUP. Cross builds are compile
or emulated-runtime evidence, never device evidence. ARM32 requires a confirmed
SDK, CPU, OS and ABI. Device runtime and physical power-loss testing remain
explicitly deferred; no CI or Windows runtime success is inferred from local
runs. The existing storage/flush assumptions and uncertain-commit contract apply.
