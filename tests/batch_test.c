#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "timelite.h"
#include "file_io.h"
#include "test_assert.h"
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
    struct timelite_db legacy;
    struct timelite_file file = TIMELITE_FILE_INIT;
    struct timelite_record input[2] = {{0, UINT64_MAX, INT64_MIN},
                                     {UINT32_MAX, 0, INT64_MAX}};
    struct timelite_record output[2];
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    unsigned char saved[240], damaged;
    uint64_t sequence = 99, size;
    size_t count = 99, transferred, prefix;
    int error;
    assert(make_test_directory(directory, sizeof(directory)));
    assert(snprintf(path, sizeof(path), "%s/db-\xc5\xbe", directory) < (int)sizeof(path));
    assert(snprintf(wal_path, sizeof(wal_path), "%s/wal-\xc5\xbe", directory) < (int)sizeof(wal_path));
    assert(timelite_batches_init(&db) == 0);
    assert(timelite_batches_close(&db) == EBADF);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, 0) == TIMELITE_BUFFER_TOO_SMALL);
    error = timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                  scratch, sizeof(scratch));
    if (error == ENOTSUP)
    {
        /* Private directory and known names, no foreign files can be removed. */
        (void)remove_test_file(path);
        (void)remove_test_file(wal_path);
        assert(remove_test_directory(directory) == 0);
        puts("batch native: unsupported provisioning correctly rejected; behavior tested by model");
        return 77;
    }
#if defined(_WIN32)
    /* Windows has no namespace durability protocol; reaching here is a bug. */
    assert(error == ENOTSUP);
#endif
    assert(error == 0);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == TIMELITE_END);
    assert(count == 99 && sequence == 99);
    assert(timelite_batches_append(&db, input, 0, scratch, sizeof(scratch),
                                  &sequence) == EINVAL);
    assert(timelite_batches_append(&db, input, SIZE_MAX, scratch, sizeof(scratch),
                                  &sequence) == EINVAL);
    assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(sequence == 1);
    assert(timelite_batches_next(&db, output, 1, &count, &sequence,
                                scratch, sizeof(scratch)) == TIMELITE_BUFFER_TOO_SMALL);
    assert(count == 99);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0);
    assert(count == 2 && sequence == 1 && output[0].value == INT64_MIN &&
           output[1].value == INT64_MAX && output[0].timestamp_us == UINT64_MAX &&
           output[1].series == UINT32_MAX);
    assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(sequence == 2);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 2);
    assert(timelite_batches_rewind(&db) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 1);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_file_open(&file, wal_path) == 0);
    assert(timelite_file_read(&file, 0, saved, sizeof(saved), &transferred) == 0);
    assert(transferred == sizeof(saved));
    assert(memcmp(saved, "TIMEWAL!\2\0\0\0", 12) == 0);
    assert(memcmp(saved + 32, "TLBATCH!\1\0\0\0\0\0\0\0\2\0\0\0H\0\0\0", 24) == 0);
    assert(saved[64] == 0 && saved[68] == 255 && saved[83] == 128);
    assert(memcmp(saved + 104, "TLCOMMIT", 8) == 0);
    assert(timelite_file_close(&file) == 0);
    /* Every second-frame write prefix: short framing fails closed; a validated
     * header and incomplete body/commit is safely discarded under the model. */
    for (prefix = 0; prefix <= 104; prefix++)
    {
        TEST_CASE("frame prefix or corruption byte", prefix);
        assert(timelite_file_open(&file, wal_path) == 0);
        assert(timelite_file_write(&file, 0, saved, sizeof(saved), &transferred) == 0);
        assert(timelite_file_truncate(&file, 136 + prefix) == 0);
        assert(timelite_file_close(&file) == 0);
        error = timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                      scratch, sizeof(scratch));
        assert(error == (prefix > 0 && prefix < 32 ? TIMELITE_INVALID_DATABASE : 0));
        if (error == 0)
        {
            assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                        scratch, sizeof(scratch)) == 0 && sequence == 1);
            assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
            assert(sequence == (prefix == 104 ? 3 : 2));
            assert(timelite_batches_close(&db) == 0);
        }
    }
    /* Every byte in committed framing/payload/commit detects single-bit damage. */
    for (prefix = 32; prefix < sizeof(saved); prefix++)
    {
        TEST_CASE("frame prefix or corruption byte", prefix);
        assert(timelite_file_open(&file, wal_path) == 0);
        assert(timelite_file_write(&file, 0, saved, sizeof(saved), &transferred) == 0);
        assert(timelite_file_truncate(&file, sizeof(saved)) == 0);
        damaged = (unsigned char)(saved[prefix] ^ 1);
        assert(timelite_file_write(&file, prefix, &damaged, 1, &transferred) == 0);
        assert(timelite_file_close(&file) == 0);
        assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                    scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
        assert(timelite_file_open(&file, wal_path) == 0);
        assert(timelite_file_size(&file, &size) == 0 && size == sizeof(saved));
        assert(timelite_file_close(&file) == 0);
    }
    assert(remove_test_file(path) == 0);
    assert(remove_test_file(wal_path) == 0);
    assert(timelite_init(&legacy) == 0);
    assert(timelite_open(&legacy, path, TIMELITE_CREATE_NEW) == 0);
    assert(timelite_close(&legacy) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_OR_CREATE,
                                scratch, sizeof(scratch)) == TIMELITE_UNSUPPORTED_VERSION);
    assert(remove_test_file(path) == 0);
    assert(remove_test_directory(directory) == 0);
    puts("batch native behavior, truncation prefixes and corruption: passed");
    return 0;
}
