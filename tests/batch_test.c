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

static void encode_fixture(unsigned char *bytes, uint64_t value, size_t width)
{
    size_t i;
    for (i = 0; i < width; i++)
    {
        bytes[i] = (unsigned char)(value >> (8 * i));
    }
}

/* Independent CRC-32 so fixtures do not depend on library internals. */
static uint32_t fixture_crc(const unsigned char *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int j;
    for (i = 0; i < length; i++)
    {
        crc ^= bytes[i];
        for (j = 0; j < 8; j++)
        {
            crc = (crc & 1) ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
        }
    }
    return ~crc;
}

/* Writes a sparse database: generation 16 manifest in slot 0, 15 maximal
 * segments and one shorter, 50000 frames each, data_end 100 bytes below the
 * capacity. Frame bodies are never read by the test. Returns an errno value
 * when the storage refuses the sparse file. */
static int capacity_fixture(struct timelite_file *file, unsigned char *scratch)
{
    uint64_t data_end = TIMELITE_DATABASE_CAPACITY - 100, offset = 160, body;
    size_t transferred;
    unsigned int i;
    int error;
    memset(scratch, 0, 64);
    memcpy(scratch, "TLINSTAL", 8);
    encode_fixture(scratch + 8, 16, 8);
    encode_fixture(scratch + 16, data_end, 8);
    encode_fixture(scratch + 24, UINT64_C(800000), 8);
    encode_fixture(scratch + 60, fixture_crc(scratch, 60), 4);
    error = timelite_file_write(file, 32, scratch, 64, &transferred);
    memset(scratch, 0, 64);
    if (error == 0)
    {
        error = timelite_file_write(file, 96, scratch, 64, &transferred);
    }
    for (i = 0; error == 0 && i < 16; i++)
    {
        body = i < 15 ? TIMELITE_WAL_CAPACITY - 32 : data_end - offset - 32;
        memcpy(scratch, "TLSEGMNT", 8);
        encode_fixture(scratch + 8, 1 + UINT64_C(50000) * i, 8);
        encode_fixture(scratch + 16, UINT64_C(50000) * (i + 1), 8);
        encode_fixture(scratch + 24, body, 4);
        encode_fixture(scratch + 28, fixture_crc(scratch, 28), 4);
        error = timelite_file_write(file, offset, scratch, 32, &transferred);
        offset += 32 + body;
    }
    if (error == 0)
    {
        assert(offset == data_end);
        error = timelite_file_truncate(file, data_end);
    }
    if (error == 0)
    {
        error = timelite_file_sync(file);
    }
    if (error == ENOSPC || error == EFBIG || error == ENOTSUP)
    {
        return error;
    }
    assert(error == 0);
    return 0;
}

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
    unsigned char saved[240], installed[304], damaged;
    uint64_t sequence = 99, size;
    size_t count = 99, transferred, prefix;
    int error, skipped = 0;
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
        /* No pair can open here, so checkpoint has no reachable path: the
         * contract result is the open ENOTSUP, then EBADF on the closed handle. */
        assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == EBADF);
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
    /* Checkpoint: install both batches, read installed then WAL data, reopen. */
    assert(timelite_file_open(&file, wal_path) == 0);
    assert(timelite_file_write(&file, 0, saved, sizeof(saved), &transferred) == 0);
    assert(timelite_file_truncate(&file, sizeof(saved)) == 0);
    assert(timelite_file_close(&file) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_size(&file, &size) == 0 && size == 160 + 32 + 208);
    assert(timelite_file_read(&file, 96, installed, sizeof(installed), &transferred) == 0);
    assert(transferred == sizeof(installed));
    assert(memcmp(installed, "TLINSTAL\1\0\0\0\0\0\0\0\x90\1\0\0\0\0\0\0\2\0\0\0\0\0\0\0", 32) == 0);
    assert(memcmp(installed + 64, "TLSEGMNT\1\0\0\0\0\0\0\0\2\0\0\0\0\0\0\0\xd0\0\0\0", 28) == 0);
    assert(memcmp(installed + 96, saved + 32, 208) == 0);
    assert(timelite_file_close(&file) == 0);
    assert(timelite_file_open(&file, wal_path) == 0);
    assert(timelite_file_size(&file, &size) == 0 && size == 32);
    assert(timelite_file_close(&file) == 0);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 1);
    assert(count == 2 && output[0].value == INT64_MIN && output[1].series == UINT32_MAX);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 2);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == TIMELITE_END);
    /* Append after reclaim continues the sequence and becomes visible at END. */
    assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(sequence == 3);
    assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 3);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    /* Bounded fill variant: five append/checkpoint cycles. The full 64 MiB
     * fill across checkpoints runs in the model test; here every append costs
     * two device flushes, so the native variant stays small by design. */
    for (prefix = 0; prefix < 5; prefix++)
    {
        TEST_CASE("append and checkpoint cycle", prefix);
        assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(sequence == 5 + 2 * prefix);
        assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    }
    for (prefix = 0; prefix < 2; prefix++)
    {
        TEST_CASE("read all after cycles", prefix);
        assert(timelite_batches_rewind(&db) == 0);
        for (sequence = 1; sequence <= 13; sequence++)
        {
            uint64_t found = 0;
            assert(timelite_batches_next(&db, output, 2, &count, &found,
                                        scratch, sizeof(scratch)) == 0);
            assert(found == sequence && count == (sequence >= 4 && sequence % 2 == 1 ? 1 : 2));
        }
        assert(timelite_batches_next(&db, output, 2, &count, &sequence,
                                    scratch, sizeof(scratch)) == TIMELITE_END);
        assert(timelite_batches_close(&db) == 0);
        assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                    scratch, sizeof(scratch)) == 0);
    }
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_file_open(&file, wal_path) == 0);
    assert(timelite_file_size(&file, &size) == 0 && size == 32);
    assert(timelite_file_close(&file) == 0);
    assert(remove_test_file(path) == 0);
    assert(remove_test_file(wal_path) == 0);
    /* Fresh pair with one installed single-record frame (276-byte database).
     * Bit flips: slot 0 (older manifest) still opens; slot 1 and the segment
     * header fail closed at open; the installed frame fails closed at read. */
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_read(&file, 0, installed, 276, &transferred) == 0 && transferred == 276);
    assert(timelite_file_close(&file) == 0);
    for (prefix = 32; prefix < 276; prefix++)
    {
        TEST_CASE("installed byte flip", prefix);
        assert(timelite_file_open(&file, path) == 0);
        assert(timelite_file_write(&file, 0, installed, 276, &transferred) == 0);
        damaged = (unsigned char)(installed[prefix] ^ 1);
        assert(timelite_file_write(&file, prefix, &damaged, 1, &transferred) == 0);
        assert(timelite_file_close(&file) == 0);
        error = timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                      scratch, sizeof(scratch));
        if (prefix >= 96 && prefix < 192)
        {
            assert(error == TIMELITE_INVALID_DATABASE);
            continue;
        }
        assert(error == 0);
        count = 99;
        error = timelite_batches_next(&db, output, 2, &count, &sequence,
                                      scratch, sizeof(scratch));
        assert(error == (prefix < 96 ? 0 : TIMELITE_INVALID_DATABASE));
        assert(count == (prefix < 96 ? 1 : 99));
        assert(timelite_batches_close(&db) == 0);
    }
    /* Database cut below data_end with an empty WAL fails closed. */
    for (prefix = 161; prefix < 276; prefix++)
    {
        TEST_CASE("database cut", prefix);
        assert(timelite_file_open(&file, path) == 0);
        assert(timelite_file_write(&file, 0, installed, 276, &transferred) == 0);
        assert(timelite_file_truncate(&file, prefix) == 0);
        assert(timelite_file_close(&file) == 0);
        assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                    scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
    }
    /* Capacity: a sparse fixture with 16 segments ending 100 bytes below the
     * limit. Checkpoint refuses before any effect; append and close still work. */
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_write(&file, 0, installed, 276, &transferred) == 0);
    error = capacity_fixture(&file, installed);
    assert(timelite_file_close(&file) == 0);
    if (error == 0)
    {
        assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                    scratch, sizeof(scratch)) == 0);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(sequence == 800001);
        assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == TIMELITE_DATABASE_FULL);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(sequence == 800002);
        assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == TIMELITE_DATABASE_FULL);
        assert(timelite_batches_close(&db) == 0);
        assert(timelite_file_open(&file, path) == 0);
        assert(timelite_file_size(&file, &size) == 0 && size == TIMELITE_DATABASE_CAPACITY - 100);
        assert(timelite_file_close(&file) == 0);
        assert(timelite_file_open(&file, wal_path) == 0);
        assert(timelite_file_size(&file, &size) == 0 && size == 32 + 2 * 84);
        assert(timelite_file_close(&file) == 0);
    }
    else
    {
        printf("SKIP capacity fixture: sparse 1 GiB file failed with %d\n", error);
        skipped = 1;
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
    if (skipped)
    {
        /* Exit 78 tells the runner a required group did not run. */
        puts("batch native behavior, checkpoint and corruption: passed, capacity group SKIPPED");
        return 78;
    }
    puts("batch native behavior, checkpoint, truncation prefixes and corruption: passed");
    return 0;
}
