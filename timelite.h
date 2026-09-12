#ifndef TIMELITE_H
#define TIMELITE_H

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

#endif
