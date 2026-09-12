/* Reports whether durable batch provisioning works in a directory.
 * Exit 0: supported. Exit 77: ENOTSUP (contract). Exit 1: any other error.
 * The runner uses this before trusting a storage root for native batch tests. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "timelite.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include "windows_test_paths.h"
#else
#include <unistd.h>
#include "posix_test_paths.h"
#define remove_test_file unlink
#define remove_test_directory rmdir
#endif

int main(void)
{
#if defined(_WIN32)
    char directory[MAX_PATH];
#else
    char directory[4096];
#endif
    char path[4608], wal_path[4608];
    struct timelite_batches db;
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    int error, status = 1;

    if (!make_test_directory(directory, sizeof(directory)))
    {
        fputs("probe: cannot create private directory\n", stderr);
        return 1;
    }
    if (snprintf(path, sizeof(path), "%s/probe-db", directory) >= (int)sizeof(path) ||
        snprintf(wal_path, sizeof(wal_path), "%s/probe-wal", directory) >= (int)sizeof(wal_path))
    {
        (void)remove_test_directory(directory);
        return 1;
    }
    (void)timelite_batches_init(&db);
    error = timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                  scratch, sizeof(scratch));
    if (error == 0)
    {
        status = timelite_batches_close(&db) == 0 ? 0 : 1;
        puts("probe: durable provisioning supported");
    }
    else if (error == ENOTSUP)
    {
        status = 77;
        puts("probe: durable provisioning unsupported (ENOTSUP)");
    }
    else
    {
        printf("probe: open failed with %d\n", error);
    }
    /* Private directory with known names; nothing foreign can be removed. */
    (void)remove_test_file(path);
    (void)remove_test_file(wal_path);
    if (remove_test_directory(directory) != 0)
    {
        fputs("probe: directory cleanup failed\n", stderr);
        return 1;
    }
    return status;
}
