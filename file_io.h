#ifndef TIMELITE_FILE_IO_H
#define TIMELITE_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

/* Internal Linux/macOS/Windows API. Not part of timelite.h. */
struct timelite_file
{
#if defined(_WIN32)
    void *handle;
#else
    int fd;
#endif
};

#if defined(_WIN32)
#define TIMELITE_FILE_INIT { NULL }
#else
#define TIMELITE_FILE_INIT { -1 }
#endif

/* Initialize with TIMELITE_FILE_INIT. Do not copy an open handle, modify it,
 * close it externally, or share it across concurrent calls. Buffers and output
 * pointers must be valid and must not overlap the handle or each other.
 * All functions return 0 or an errno value (not -1). Windows maps native errors
 * to errno values; unknown errors become EIO. See feature 002 for the mapping.
 * Open/create require a closed handle and leave it closed on failure. A create
 * that fails after opening may leave a new empty path; cleanup errors during
 * open/create do not replace the original error. No path is removed here.
 * POSIX: files are opened read/write, close-on-exec; create uses 0600 with umask.
 * Open follows symlinks; create refuses any existing path. Use trusted paths.
 * Only regular files are accepted. No locks or multi-process protection.
 * Failed read/write/size/truncate/sync retain ownership of the handle; a failed
 * transfer or truncate may change data or position. Close it when finished. */
/* Windows: UTF-8 paths, at most 259 bytes excluding NUL, checked conversion to
 * UTF-16. Invalid encoding returns EILSEQ, excess length ENAMETOOLONG. Normal
 * Win32 path rules apply; no long-path prefix is added. Use trusted file paths.
 * Handles are non-inheritable. Default inherited directory ACLs apply on create.
 * Share read/write/delete: compatible opens, rename and deletion are allowed;
 * other handles can still deny sharing. No general multi-process protection. */
int timelite_file_open(struct timelite_file *file, const char *path);
int timelite_file_create(struct timelite_file *file, const char *path);

/* transferred is required and set to zero before validation. A zero length
 * permits a NULL buffer. Offset + length must fit INT64_MAX, even for reads.
 * Read fills the request or stops at EOF (success with a smaller count).
 * Write loops until complete; zero progress returns EIO. On error the count
 * reports completed bytes; the buffer/file prefix may already have changed.
 * Failed writes are not rolled back. No operation implicitly syncs. */
int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *transferred);
int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *transferred);
/* Size output is unchanged on failure. Truncate may shrink or zero-extend.
 * Failures give no rollback or durability guarantee; inspect before reuse. */
int timelite_file_size(struct timelite_file *file, uint64_t *size);
int timelite_file_truncate(struct timelite_file *file, uint64_t size);
/* Linux: fdatasync. macOS: F_FULLFSYNC, without a weaker fallback.
 * Windows: FlushFileBuffers, without a weaker fallback. Close is not sync.
 * Does not sync the parent directory or guarantee power-loss recovery. */
int timelite_file_sync(struct timelite_file *file);
/* Consumes the handle even on error. Never retried, including EINTR.
 * EINTR may leave a descriptor open on some systems; do not reuse/retry it.
 * Windows: calls CloseHandle once, clears handle to NULL even on failure.
 * Failure does not promise OS resource release; no automatic retry is made.
 * Close is not sync. A second close returns EBADF. */
int timelite_file_close(struct timelite_file *file);

/* Feature 004: random identity and durable namespace provisioning. Provision
 * requires a stable trusted path and previously durable directory ancestry.
 * Checks the opened file against the final directory entry and syncs both.
 * Unsupported platforms/filesystems return ENOTSUP without weaker fallback. */
int timelite_file_identity(unsigned char identity[16]);
int timelite_file_provision(struct timelite_file *file, const char *path);

#endif
