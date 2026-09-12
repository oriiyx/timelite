#ifndef TIMELITE_H
#define TIMELITE_H

#include <stddef.h>
#include <stdint.h>

/* Caller-owned storage. Fields are private; do not inspect, change or copy an
 * open handle. No stable ABI across platforms/builds is promised. */
struct timelite_db
{
#if defined(_WIN32)
    void *private_handle;
#else
    int private_fd;
#endif
};

enum timelite_open_mode
{
    TIMELITE_OPEN_EXISTING,
    TIMELITE_CREATE_NEW,
    TIMELITE_OPEN_OR_CREATE
};

#define TIMELITE_INVALID_DATABASE (-1)
#define TIMELITE_UNSUPPORTED_VERSION (-2)

/* Returns 0 or a positive errno value, except for the negative format errors
 * above. Initialize fresh or closed storage before first use. NULL: EINVAL.
 * Never initialize an open handle: doing so loses its resource. Uninitialized
 * or modified handles are caller errors that cannot be reliably detected. */
int timelite_init(struct timelite_db *db);

/* Requires an initialized closed handle, trusted nonempty path, and valid mode;
 * otherwise EINVAL. One owner must serialize all calls; no process locking.
 * Opens read/write without truncation. CREATE_NEW refuses existing paths.
 * OPEN_OR_CREATE creates only after ENOENT, exclusively; if another creator
 * wins, opens and validates once, without waiting for initialization or repair.
 * Invalid/empty/short/v1 trailing data: INVALID_DATABASE. Nonzero unknown format
 * version: UNSUPPORTED_VERSION. Existing files are never written by open.
 * Argument rejection leaves the handle unchanged (including an open handle).
 * Otherwise success owns an open resource; failure leaves a closed reusable handle;
 * cleanup close errors never replace the primary error. OS release can be
 * uncertain on close failure. Failed creation can leave an empty, partial, or
 * valid file; no file is deleted. Valid artifacts can reopen after failed sync.
 * Creation writes the header and syncs the file, but does not sync its directory:
 * success is NOT a promise of durable creation under power loss.
 * Windows paths are UTF-8, at most 259 bytes excluding NUL; POSIX native bytes.
 * No path/buffer is retained. See feature 003 for the exact format and limits. */
int timelite_open(struct timelite_db *db, const char *path,
                  enum timelite_open_mode mode);

/* No implicit sync. Consumes the handle on success or failure; never retry a
 * failed native close. Resource release is uncertain on failure. The closed
 * handle can be reused. NULL: EINVAL; already closed: EBADF. */
int timelite_close(struct timelite_db *db);

/* The returned string belongs to the library. Do not modify or free it. */
const char *timelite_version(void);

/* Version 2 WAL batch API. Separate handle preserves the v1 lifecycle API.
 * All fields private. Initialize fresh/closed storage, never copy an open handle.
 * Input, output, scratch and handles must not overlap. No buffers are retained. */
#define TIMELITE_MAX_RECORDS 64u
#define TIMELITE_BATCH_SCRATCH 1344u
#define TIMELITE_WAL_CAPACITY UINT64_C(67108864)
#define TIMELITE_END (-3)
#define TIMELITE_BUFFER_TOO_SMALL (-4)
#define TIMELITE_RECOVERY_REQUIRED (-5)
#define TIMELITE_WAL_FULL (-6)
#define TIMELITE_PAIR_MISMATCH (-7)

struct timelite_record
{
    uint32_t series;
    uint64_t timestamp_us;
    int64_t value;
};

struct timelite_batches
{
#if defined(_WIN32)
    void *private_database;
    void *private_wal;
#else
    int private_database;
    int private_wal;
#endif
    uint64_t private_end;
    uint64_t private_sequence;
    uint64_t private_cursor;
    uint64_t private_read_sequence;
    int private_failed;
};

int timelite_batches_init(struct timelite_batches *db);
/* Explicit distinct trusted paths, stable through close. Directory ancestry must
 * already be durable. Always re-establishes file and directory durability, even
 * on reopen. No missing-member repair. Creation may leave partial files; none
 * are deleted. Rejects v1 without migration. Scratch >= BATCH_SCRATCH required.
 * Complete corruption fails closed; only validated incomplete suffixes are
 * truncated and synced. Failed opens consume resources and preserve first error.
 * Windows creation returns ENOTSUP before file effects; otherwise-valid existing
 * pairs fail provisioning with ENOTSUP. */
int timelite_batches_open(struct timelite_batches *db, const char *database_path,
                          const char *wal_path, enum timelite_open_mode mode,
                          void *scratch, size_t scratch_size);
/* 1..64 records, all integer values valid, caller chooses value units. Timestamps
 * are unsigned microseconds since Unix epoch. Order and duplicates preserved.
 * Sequence output written only on durable success. Argument/capacity errors are
 * harmless; any write/sync failure requires close/reopen before reads or appends.
 * Failed append may commit; enumerate after reopen to reconcile, no exactly-once
 * retry guarantee. WAL capacity is fixed and cannot be reclaimed until 005. */
int timelite_batches_append(struct timelite_batches *db,
                            const struct timelite_record *records, size_t count,
                            void *scratch, size_t scratch_size, uint64_t *sequence);
/* Validated output only on success. All outputs/cursor unchanged on error or END.
 * BUFFER_TOO_SMALL allows retry at same cursor. Scratch >= BATCH_SCRATCH required.
 * At END, later serialized appends become visible. Rewind returns to batch 1. */
int timelite_batches_next(struct timelite_batches *db,
                          struct timelite_record *records, size_t capacity,
                          size_t *count, uint64_t *sequence,
                          void *scratch, size_t scratch_size);
int timelite_batches_rewind(struct timelite_batches *db);
/* Consumes both resources even if one close fails; returns first error. */
int timelite_batches_close(struct timelite_batches *db);

#endif
