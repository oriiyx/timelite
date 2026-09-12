# Timelite: feasibility of a small embedded time-series database

Research date: 11 September 2026. Status: research and proposed proof-of-concept scope only; no implementation or
benchmarks performed. Rust is excluded as the implementation language.

## Concise conclusion

**The idea is feasible. My recommendation is a C core with a small C API, with Odin bindings and tooling if desired.**
If the initial product deliberately targets a verified Odin-supported Linux platform, an Odin core is also a reasonable
choice. C is the stronger default for broad device support and easy adoption into existing firmware; it is not
guaranteed to produce the smallest binary in every comparison.

Aim for **SQLite's embedding experience, specialized for sensor history**: link a library, open local storage, append
batches, read time ranges, and expire old data. Reproducing SQLite's SQL, file format, or API compatibility would
greatly enlarge the project and is unnecessary for that experience.

Start with the company's actual Linux device and workload. Treat microcontrollers as a separate porting milestone. The
first question to prove is whether specialization materially improves storage consumption, write traffic, bounded
memory, or retention compared with well-configured SQLite. A 500 MB RAM budget alone does not establish a need to
replace SQLite.

The largest engineering commitment is dependable recovery after interrupted writes and storage reuse. A fast append
demonstration is achievable much sooner than a database people can entrust with field data.

## 1. Define which embedded devices this means

“Embedded” covers substantially different environments. These are illustrative classes, not product limits:

| Device class                | Typical environment                                            | Consequence for this project                                                                                 |
|-----------------------------|----------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------|
| Linux gateway or controller | Tens/hundreds of MB of RAM; filesystem; SD/eMMC                | Start here given the stated example. File I/O and a host application already exist.                          |
| Microcontroller             | KB to a few MB of RAM; bare metal or RTOS; often raw NOR flash | Fixed buffers, CPU/toolchain support, erase geometry, and storage drivers become primary design constraints. |
| Very small 8/16-bit device  | Severe address-space and memory limits                         | Separate scope; do not promise support in the first design.                                                  |

A 1 GHz single-core processor tells us little about storage latency or endurance. The missing facts that matter most are
CPU architecture/ABI, OS, free memory available to the library, storage medium, sample rate, retention period, and
acceptable loss after power failure.

**Working assumption:** the first target is a Linux device with a filesystem, one application owning the database, and
numeric sensor readings. This remains a proposal until the company's hardware is identified.

The preference for avoiding a database server is sensible for this deployment. We do not need a universal claim that
PostgreSQL cannot run with 500 MB to justify an in-process library; SQLite is the relevant incumbent here.

## 2. How SQLite is embedded, at the macro level

SQLite is executable library code inside your application. It does not require a database server process or network
connection. The application calls functions, and the library accesses local storage. SQLite's official description calls
it an in-process, serverless engine. [SQLite overview](https://www.sqlite.org/about.html)

```text
Device application — one process
  Sensor collection
       |
       v
  Database function calls
       |
       v
  Linked SQLite library
       |
       v
  OS / filesystem ---> persistent storage
```

There are three common integration arrangements:

1. **Compile the source into the application.** SQLite distributes an amalgamation, `sqlite3.c`, plus its public header,
   `sqlite3.h`. The compiler and linker incorporate the database into the
   application. [SQLite amalgamation](https://www.sqlite.org/amalgamation.html)
2. **Link a library.** Static linking incorporates the needed library code into the executable; dynamic linking loads a
   separately distributed shared library at runtime. Neither requires a server.
3. **Use a language binding.** A wrapper in the application's language translates calls into the native interface. This
   does not require rewriting the database in that language. SQLite explains cross-language compatibility as a major
   reason for its C implementation. [Why SQLite uses C](https://www.sqlite.org/whyc.html)

At runtime the normal lifecycle is open, prepare an operation, bind its inputs, execute/read results, release the
operation, and close. An open “connection” is a library handle, not a TCP connection. The application need not load the
entire database into
RAM. [SQLite C interface](https://www.sqlite.org/cintro.html), [SQLite memory management](https://www.sqlite.org/malloc.html)

Two distinctions matter:

- **One database file does not mean exactly one file exists during operation.** SQLite can create a rollback journal, or
  WAL and shared-memory sidecars. Copying only the main file while writes are active is not a general backup
  procedure. [SQLite temporary files](https://www.sqlite.org/tempfiles.html), [SQLite WAL](https://www.sqlite.org/wal.html)
- **Embedded library does not automatically mean bare-metal ready.** Storage and platform services must be provided.
  SQLite has a VFS boundary for OS-facing operations. Timelite would need its own smaller storage
  boundary. [SQLite VFS](https://www.sqlite.org/vfs.html)

Timelite can offer the same integration pattern with operations such as `open`, `append_batch`, `scan_range`, `sync`,
and `close`. SQL is independent of embedding.

## 3. Which implementation language?

These rankings are engineering judgments for this project, not measured binary-size results.

| Language | Fit for this goal                                                                            | Main tradeoff                                                                           | Recommendation                                                                    |
|----------|----------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------|
| **C**    | Native library, explicit allocation, broad firmware integration, straightforward C interface | Memory safety, ownership, bounds checks, and cleanup require discipline                 | **Default choice for the core**                                                   |
| **Odin** | Native code, allocator control, C interoperability; attractive given your preference         | Exact target support and distribution into customers' build systems need validation     | Good candidate for a deliberately scoped target; excellent binding/tooling option |
| **Zig**  | Explicit allocators, freestanding support, C ABI integration                                 | Must validate the selected toolchain version and target; adds another language to learn | Credible alternative if you want to evaluate it                                   |
| **C++**  | Native library with useful resource-management abstractions                                  | Requires a constrained dependency/runtime policy and a C boundary for consumers         | Viable if you prefer C++; no automatic size advantage over C                      |

C's case is integration and portability rather than a claim that other native languages are inherently bloated. SQLite
itself emphasizes compatibility, few dependencies, and stability. [SQLite's rationale](https://www.sqlite.org/whyc.html)

Zig documents explicit allocator parameters, freestanding use, and C ABI interoperability. Those fit the problem, but
its published small executable examples are not database benchmarks. [Zig overview](https://ziglang.org/learn/overview/)

C++ need not bring a large framework, but exceptions and standard-library choices affect dependencies and binary size.
Disabling exceptions also affects error handling; it is not just a free optimization
switch. [GCC exception documentation](https://gcc.gnu.org/onlinedocs/libstdc%2B%2B/manual/using_exceptions.html)

### Odin specifically

Odin should not be dismissed just because the project is embedded. It supports C calling conventions, configurable
allocators, and library build modes. A C-facing boundary must explicitly manage context and expose ABI-compatible types
rather than Odin-specific slices or implicit calling
conventions. [Odin C interoperability](https://odin-lang.org/news/binding-to-c/), [Odin overview](https://odin-lang.org/docs/overview/), [Compiler flags](https://github.com/odin-lang/Odin/wiki/Compiler-Flags)

**Target support needs qualification.** The FAQ lists AMD64, ARM64, and WebAssembly architectures, while the compiler's
current target table also includes `linux_arm32`, `freestanding_arm32`, and RISC-V entries. These sources are not fully
aligned. A target-table entry does not establish that your exact CPU, floating-point ABI, runtime, linker setup, and
required libraries work. Do not infer blanket MCU support, or blanket ARM32 incompatibility, from one
page. [Odin FAQ](https://odin-lang.org/docs/faq/), [Compiler target definitions](https://github.com/odin-lang/Odin/blob/master/src/build_settings.cpp)

For a confirmed ARM64 Linux product, I would seriously consider an Odin core if that keeps you productive. For a library
intended to drop into many vendors' existing firmware builds, I would choose C and expose a pleasant Odin wrapper. A C
ABI makes calling an Odin library easier; it does not remove the need to build that library for each target.

### What “small” must measure

Measure separately: added executable/firmware bytes, static RAM, peak stack, peak heap, storage bytes per sample, and
physical write traffic where measurable. Include initialization, recovery, queries, and retention in memory
measurements.

Compare equivalent release builds on the same architecture, using the same functionality and durability guarantees.
Include dependencies pulled into the final application. A small archive or a hello-world executable is not a useful
prediction of database footprint.

SQLite's published size examples are roughly 590–750 KB for specific 2023 builds; those are dated examples, not a
universal current measurement. They show why “smaller than SQLite” needs an actual
baseline. [SQLite footprint](https://www.sqlite.org/footprint.html)

## 4. Where specialization could help

The proposed advantage comes from restricting operations and exploiting recurring sensor patterns:

| Workload property           | Opportunity                                      | Limitation                                              |
|-----------------------------|--------------------------------------------------|---------------------------------------------------------|
| Mostly append-only readings | Batch sequential writes                          | Late arrivals and clock corrections complicate ordering |
| Repeated series identity    | Store metadata once and use compact IDs          | Series count must still be bounded                      |
| Regular timestamps          | Encode timestamp differences compactly           | Irregular sampling reduces the gain                     |
| Slowly changing values      | Integer deltas or floating-point XOR compression | Noisy data may compress poorly                          |
| Queries over time intervals | Skip blocks outside the interval                 | Cross-series predicates need more indexing or scans     |
| Old data expires together   | Reclaim whole storage segments                   | Retention becomes coarse at segment boundaries          |

Timestamp delta-of-delta and floating-point XOR are established techniques described in Gorilla. Its reported results
concern a different, large-scale in-memory system; they do not predict Timelite's compression ratio or durability
requirements. Start without compression, then measure codecs on actual
readings. [Gorilla paper](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf)

**A crucial product fork:** if devices only buffer records until upload, a durable bounded queue may be sufficient. A
time-series database earns its added complexity when local software needs queries such as “temperature between these
times” or “maximum over the last hour.” Confirm that need before investing in query indexes.

## 5. Existing work and the baseline to beat

This is an existing category, so novelty must come from a specific improvement.

| Candidate                   | Evidence and relevance                                                                                                                         | What to investigate                                                                                       |
|-----------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| SQLite                      | Existing incumbent; transactions, configurable memory, familiar tooling                                                                        | Whether batching, schema, cache settings, and retention design already solve the problem                  |
| FlashDB                     | C project offering KV and time-series storage, flash wear balancing and power-off protection                                                   | Timestamp rules, query capabilities, integration effort, and actual memory for the selected configuration |
| EmbedDB                     | Embedded sensor database with time-series-oriented storage and indexing; project states a 4 KB minimum memory requirement and no OS dependency | Ordering restrictions, recovery contract, exact configuration footprint, and suitability for the workload |
| Application log on littlefs | littlefs provides flash-oriented filesystem behavior and bounded-memory configuration                                                          | Whether a simple record log is enough; filesystem recovery does not define database batch atomicity       |

These are project descriptions, not independently verified performance or reliability results. FlashDB's “almost zero”
RAM statement should not be interpreted as zero total integration
memory. [FlashDB](https://github.com/armink/FlashDB), [EmbedDB](https://github.com/ubco-db/EmbedDB), [littlefs](https://github.com/littlefs-project/littlefs)

Three development paths are available:

1. **A narrow API over SQLite:** quickest way to test the product interface; retains SQLite's footprint and storage
   behavior.
2. **A specialized engine:** greatest control over size and layout; owns recovery, format compatibility, and testing.
3. **An adapter around an existing embedded engine:** may avoid substantial storage work if its constraints fit.

For this exploration, specify path 2, but keep paths 1 and 3 as explicit comparison points. Avoid starting from a fork
of SQLite: its broad machinery is not an obvious shortcut to a tiny specialized engine. Optional SQL integration can be
considered later through SQLite's extension interfaces, at the cost of retaining SQLite in that
configuration. [SQLite extension interfaces](https://www.sqlite.org/cintro.html)

## 6. What must be designed before implementation

The following is a proposed contract, not an established implementation.

### Data and API

- Fixed numeric series IDs; separately registered metadata and one value type per series.
- Explicit fixed-width on-disk fields, byte order, format version, and length limits. Never persist native structs by
  copying their memory layout.
- Timestamp unit and epoch, missing/invalid time representation, equal-timestamp behavior, and integer/float semantics.
- Separate persistent append sequence from event timestamp. Sensor time can move backward; upload progress must not
  depend on wall-clock ordering.
- Batch append, bounded-memory range iteration, latest reading, explicit sync, retention, and error reporting. Define
  whether “latest” means latest arrival or greatest event timestamp.
- One owning process with serialized operations initially. Return errors on unsupported concurrent use; additional
  readers/writers require a separate concurrency design.

For the first version, accept backward timestamps and retain append order; range results need not be sorted by event
time. That avoids rejecting clock corrections but weakens time-based skipping. If sorted results are required, budget
explicitly for ordering or impose and document input constraints.

### Durability

Specify two distinct events: **accepted into the library's buffer** and **durably committed**. A buffered append may be
lost before sync. A successful durable batch commit must recover as a complete batch after restart, subject to the
documented storage guarantees. An interrupted, unacknowledged commit may be present or absent; consumers must tolerate
retries.

Checksums detect damaged records; they do not make a sequence of writes atomic. Batch boundaries, lengths,
sequence/generation numbers, validation, and write/sync ordering need a recovery protocol. Updating or reclaiming
metadata needs the same care as appending data. SQLite's atomic-commit discussion illustrates the importance of storage
assumptions and synchronization. [SQLite atomic commit](https://www.sqlite.org/atomiccommit.html)

Do not promise persistence that the device's storage cannot provide. The test plan must include real power cuts on
representative media, not only killing the process. SQLite also documents failures caused by storage or OS behavior that
does not honor synchronization expectations. [SQLite corruption causes](https://www.sqlite.org/howtocorrupt.html)

### Resource and storage contract

Set explicit ceilings for series count, batch size, query buffer, metadata cache, and database capacity. Allocation
failure and full storage must return usable errors. Avoid an index whose RAM consumption grows with every sample.

Filesystem storage and raw flash require different adapters. Raw flash adds erase-before-write rules, program alignment,
endurance management, and potentially bad-block handling. Reusing a suitable filesystem or flash layer can reduce scope,
but its guarantees must be understood. [littlefs design and interface](https://github.com/littlefs-project/littlefs)

Define recovery time as a budget. A design using little RAM but scanning years of history at every boot can still be
unusable.

## 7. A plausible first storage architecture

**Recommendation:** a bounded set of append-only segment files on Linux, with blocks containing small batches,
checksums, and timestamp bounds. Start with uncompressed records and rebuildable indexes. This is a design direction,
not a complete crash-safe format specification.

```text
Application / language bindings
              |
          Small C API
              |
   Batch buffer + range iterator
              |
  Segmented record store + recovery
              |
        Storage adapter
              |
      Linux filesystem first
```

Use a global append sequence for traversal and resume cursors. Blocks can mix series initially; a time query skips
blocks using timestamp minimum/maximum and then filters records. This keeps initial buffering simple. Per-series blocks
could improve compression and scans later, but allocating one buffer per series can exhaust RAM.

Keep only a bounded metadata cache in memory. Store summaries on disk and recover the active tail using validated
checkpoints/segment metadata. The exact metadata protocol must avoid turning every startup into a full scan. Sealed
blocks should not be edited in place merely to append more samples.

Retention reclaims complete eligible segments, with headroom reserved for metadata changes and progress when capacity is
reached. For event-time retention, a segment is eligible only when all its relevant timestamps are outside retention;
mixed or incorrect future timestamps can delay reclamation. Capacity-based eviction is a different policy and must be
explicit. Do not silently overwrite unuploaded data by default.

**Single-file storage is possible**, using a preallocated file with reusable regions, but safely reusing those regions
and updating metadata adds complexity. A bounded directory of segments is the simpler first experiment. If a literal
single-file artifact is a hard requirement, settle it before implementation; a later snapshot/export feature is not the
same guarantee.

For upload, keep HTTP/MQTT, authentication, and retry scheduling in the application. The storage API can expose
sequence-based reads and a durable acknowledged cursor. Advance the cursor only after remote acknowledgement. A crash
between remote acceptance and local acknowledgement can cause duplicates, so use stable record identities and receiver
deduplication. If retention overtakes a cursor, return an explicit gap rather than silently skipping data.

## 8. The proof of concept to run later

No code is needed for this research stage. The next authorized implementation phase should answer a small number of
measurable questions.

### Establish the workload

Collect the actual board/CPU/OS/storage, the current SQLite schema and settings, a representative trace, series count,
sustained/burst sampling rate, common queries, offline duration, and durability requirement.

Storage grows quickly even before database overhead. As an illustrative calculation, 100 series at one sample/second
produce 8.64 million samples/day. At 20 encoded bytes/sample, that is **172.8 MB/day or 5.184 GB/30 days**, excluding
indexes, metadata, journals, and filesystem overhead. These are decimal units and hypothetical record sizes, not
Timelite measurements.

### Use a fair SQLite baseline

Reuse prepared inserts, batch transactions, and index the actual lookup pattern. Compare a composite primary key in a
`WITHOUT ROWID` table where appropriate; that layout can help some composite-key workloads but is not universally
better. Include retention cost and on-disk
sidecars. [Prepared statement reuse](https://www.sqlite.org/cintro.html), [WITHOUT ROWID](https://www.sqlite.org/withoutrowid.html)

Match durability and batching. For example, SQLite WAL with `synchronous=FULL` is an appropriate durable-commit baseline
when the storage honors synchronization. WAL with `NORMAL` can lose recent commits after power loss, so it belongs in a
separately labeled relaxed-durability comparison. Do not compare a RAM-buffered append against a synced SQLite
transaction and call the difference an engine
speedup. [SQLite synchronous settings](https://www.sqlite.org/pragma.html#pragma_synchronous)

### Sequence the experiments

| Stage                             | Question                                                                     | Evidence needed                                                                 |
|-----------------------------------|------------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| 1. Target and language check      | Can the chosen language produce an embeddable library for the actual device? | C host calls it; dependency and linked-size report; target execution            |
| 2. Minimal durable store          | Can batches append and survive restart within a memory budget?               | Round-trip correctness, memory high-water marks, interrupted-write recovery     |
| 3. Time-range reads and retention | Can useful queries and bounded storage coexist?                              | Query correctness, query latency, steady-state capacity, reclaim/recovery tests |
| 4. Workload comparison            | Does specialization provide a worthwhile improvement?                        | SQLite and alternative-engine comparison at equal semantics                     |
| 5. Compression, only if justified | Does encoding reduce total cost on real readings?                            | Storage reduction plus CPU, latency, and memory overhead                        |

Record linked code size, heap/stack peaks, bytes per sample, logical write bytes, durable batch latency, query latency,
and startup/recovery time. Distinguish application write traffic from actual flash writes; controller amplification may
not be observable. Include cold-cache runs and enough data for retention to reach steady state.

Illustrative initial Linux experiment targets could be under **256 KiB added code** and under **1 MiB database-owned
RAM**, with explicit stack accounting. These are proposed goals, not predictions or MCU promises. Select throughput and
latency thresholds from the application requirements. A useful continuation criterion might be a twofold reduction in
storage or write traffic, or a required memory/retention bound SQLite cannot meet, without unacceptable regressions.

Correctness is a prerequisite, not a benchmark category to trade away. Test short writes, full storage, failed syncs,
allocation failures, malformed lengths, truncated batches, clock rollback, and interruption during segment
creation/deletion/reuse. Previously acknowledged retained batches must remain readable, and incomplete batches must
never appear partially committed. Fuzz the format decoder and compare query results to a simple reference model.
SQLite's testing program shows how much effort a dependable storage engine
demands. [SQLite testing](https://www.sqlite.org/testing.html)

## 9. Expected effort and decision

Planning judgment for one experienced systems developer, rather than a delivery estimate: a constrained append/read
experiment is a weeks-scale task; credible recovery, retention, bindings, and device validation make this a months-scale
effort. Broad portability, stable upgrades, and field confidence require continuing maintenance. The estimate changes
substantially with the chosen storage medium and prior storage-engine experience.

The skills required are native library/ABI design, binary formats, file and flash I/O, crash recovery, memory budgeting,
testing/fuzzing, and performance measurement. A SQL parser or distributed-systems stack is unnecessary for the proposed
first scope.

**Recommended decision:** proceed with the research-defined concept of a small durable sensor-history library. Prefer C
for broad embedded adoption; keep Odin as a serious core option when the actual supported target is narrow and verified.
Before building the engine, establish whether the application needs time queries or just an upload queue, and obtain the
incumbent SQLite workload. That evidence should determine whether Timelite becomes a new storage engine, a thin API over
an existing one, or a smaller queue-focused project.
