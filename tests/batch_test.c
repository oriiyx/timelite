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
    encode_fixture(scratch + 40, 160 + 15 * TIMELITE_WAL_CAPACITY, 8);
    encode_fixture(scratch + 48, 7, 4);
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

/* Read errors must not publish even a partially advanced cursor. */
static void same_cursor(const struct timelite_batches *a, const struct timelite_batches *b)
{
    assert(a->private_cursor == b->private_cursor);
    assert(a->private_segment_end == b->private_segment_end);
    assert(a->private_read_sequence == b->private_read_sequence);
    assert(a->private_in_wal == b->private_in_wal);
    assert(a->private_failed == b->private_failed);
}

static void status_values(struct timelite_batches *db, uint64_t committed,
                          uint64_t installed, uint64_t segments,
                          uint64_t wal, uint64_t bytes, uint64_t timestamp)
{
    struct timelite_batches_status status;
    unsigned char before[sizeof(*db)];
    memcpy(before, db, sizeof(*db));
    assert(timelite_batches_get_status(db, &status) == 0);
    assert(status.committed_batches == committed);
    assert(status.installed_batches == installed);
    assert(status.pending_batches == committed - installed);
    assert(status.installed_segments == segments);
    assert(status.wal_bytes == wal && status.installed_bytes == bytes);
    assert(status.last_timestamp_us == timestamp);
    assert(memcmp(before, db, sizeof(*db)) == 0);
}

static void time_ranges(const char *path, const char *wal_path)
{
    struct timelite_batches db, before;
    struct timelite_record input[3] = {{7, 10, -1}, {8, 20, 2}, {7, 20, 3}};
    struct timelite_record output[3];
    struct timelite_range range = {15, 41, 7, 0};
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    const uint64_t seeks[] = {0, 10, 15, 20, 21, 30, 31, 40, 41, 50, 51};
    const uint64_t expected[] = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 0};
    uint64_t sequence = 99;
    size_t count = 99, i;
    TEST_CASE("range empty and argument rules", 0);
    assert(timelite_batches_init(&db) == 0);
    assert(timelite_batches_seek(&db, 0, scratch, sizeof(scratch)) == EBADF);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == EBADF);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, sizeof(scratch)) == 0);
    status_values(&db, 0, 0, 0, 32, 0, 0);
    assert(timelite_batches_seek(&db, 0, scratch, sizeof(scratch)) == TIMELITE_END);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_END);
    assert(count == 99 && sequence == 99);
    before = db;
    input[1].timestamp_us = 9;
    assert(timelite_batches_append(&db, input, 3, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
    same_cursor(&db, &before);
    assert(sequence == 99 && db.private_sequence == 0);
    input[1].timestamp_us = 20;
    assert(timelite_batches_append(&db, input, 3, scratch, sizeof(scratch), &sequence) == 0);
    before = db;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
    same_cursor(&db, &before);
    assert(sequence == 1 && db.private_sequence == 1);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    status_values(&db, 1, 1, 1, 32, 188, 20);
    input[0].timestamp_us = 30;
    input[1].timestamp_us = 40;
    assert(timelite_batches_append(&db, input, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    input[0].timestamp_us = 50;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    status_values(&db, 3, 2, 2, 116, 356, 50);
    input[0].timestamp_us = 49;
    before = db;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
    same_cursor(&db, &before);
    for (i = 0; i < sizeof(seeks) / sizeof(seeks[0]); i++)
    {
        TEST_CASE("seek installed and WAL boundaries", i);
        assert(timelite_batches_seek(&db, seeks[i], scratch, sizeof(scratch)) ==
               (expected[i] ? 0 : TIMELITE_END));
        assert(timelite_batches_next(&db, output, 3, &count, &sequence,
                                    scratch, sizeof(scratch)) == (expected[i] ? 0 : TIMELITE_END));
        if (expected[i])
        {
            assert(sequence == expected[i]);
        }
    }
    TEST_CASE("seek END sees later equal timestamp append", 0);
    input[0].timestamp_us = 50;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_next(&db, output, 3, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 4);
    assert(timelite_batches_seek(&db, 15, scratch, sizeof(scratch)) == 0);
    before = db;
    count = 99;
    sequence = 99;
    output[0].value = 12345;
    assert(timelite_batches_next_range(&db, &range, output, 1, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_BUFFER_TOO_SMALL);
    same_cursor(&db, &before);
    assert(count == 99 && sequence == 99 && output[0].value == 12345);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0);
    assert(sequence == 1 && count == 2 && output[0].series == 8 && output[1].series == 7);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0 && sequence == 2 && count == 2);
    before = db;
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_END);
    same_cursor(&db, &before);
    range.filter_series = 1;
    assert(timelite_batches_seek(&db, 15, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next_range(&db, &range, output, 1, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0);
    assert(sequence == 1 && count == 1 && output[0].value == 3);
    assert(timelite_batches_next_range(&db, &range, output, 1, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0);
    assert(sequence == 2 && count == 1 && output[0].timestamp_us == 30);
    /* Skipped batches are not published on a buffer error or END. */
    assert(timelite_batches_rewind(&db) == 0);
    range.from_us = 25;
    before = db;
    assert(timelite_batches_next_range(&db, &range, NULL, 0, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_BUFFER_TOO_SMALL);
    same_cursor(&db, &before);
    assert(timelite_batches_next_range(&db, &range, output, 1, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0 && sequence == 2);
    assert(timelite_batches_rewind(&db) == 0);
    before = db;
    range.series = 123;
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_END);
    same_cursor(&db, &before);
    TEST_CASE("query argument cursor preservation", 0);
    assert(timelite_batches_seek(NULL, 0, scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_seek(&db, 0, NULL, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_seek(&db, 0, scratch, 1343) == TIMELITE_BUFFER_TOO_SMALL);
    assert(timelite_batches_next_range(&db, NULL, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_next_range(&db, &range, NULL, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_next_range(&db, &range, output, 3, NULL, &sequence,
                                      scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, NULL,
                                      scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      NULL, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, 1343) == TIMELITE_BUFFER_TOO_SMALL);
    range.from_us = 42;
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == EINVAL);
    range.from_us = 41;
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == TIMELITE_END);
    range.filter_series = 2;
    assert(timelite_batches_next_range(&db, &range, output, 3, &count, &sequence,
                                      scratch, sizeof(scratch)) == EINVAL);
    same_cursor(&db, &before);
    assert(timelite_batches_close(&db) == 0);
    assert(remove_test_file(path) == 0);
    assert(remove_test_file(wal_path) == 0);
}

static void aggregate_ranges(const char *path, const char *wal_path)
{
    struct timelite_batches db, before;
    struct timelite_record records[] = {{7, 0, INT64_MIN}, {8, 0, INT64_MAX},
                                        {7, 10, -5}, {7, 10, 0}, {8, 20, 9}};
    struct timelite_range range = {0, 21, 0, 0};
    struct timelite_aggregate result;
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    uint64_t sequence;
    int phase;
    TEST_CASE("aggregate empty database", 0);
    assert(timelite_batches_init(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
    assert(result.record_count == 0 && result.minimum_value == 0 && result.maximum_value == 0);
    assert(timelite_batches_append(&db, records, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_append(&db, records + 2, 2, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_append(&db, records + 4, 1, scratch, sizeof(scratch), &sequence) == 0);
    for (phase = 0; phase < 3; phase++)
    {
        TEST_CASE("aggregate segments WAL checkpoint reopen", phase);
        if (phase == 1)
        {
            assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
        }
        if (phase == 2)
        {
            assert(timelite_batches_close(&db) == 0);
            assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                        scratch, sizeof(scratch)) == 0);
        }
        assert(timelite_batches_seek(&db, phase == 0 ? 10 : 99, scratch, sizeof(scratch)) ==
               (phase == 0 ? 0 : TIMELITE_END));
        before = db;
        range.from_us = 0;
        range.until_us = 21;
        range.filter_series = 0;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
        assert(result.record_count == 5 && result.minimum_value == INT64_MIN && result.maximum_value == INT64_MAX);
        range.until_us = 20;
        range.from_us = 10;
        range.filter_series = 1;
        range.series = 7;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
        assert(result.record_count == 2 && result.minimum_value == -5 && result.maximum_value == 0);
        range.series = 8;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
        assert(result.record_count == 0 && result.minimum_value == 0 && result.maximum_value == 0);
        range.from_us = 20;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
        assert(result.record_count == 0 && result.minimum_value == 0 && result.maximum_value == 0);
        assert(memcmp(&db, &before, sizeof(db)) == 0);
    }
    assert(timelite_batches_close(&db) == 0);
    assert(remove_test_file(path) == 0);
    assert(remove_test_file(wal_path) == 0);
}

static void legacy_range(const char *path, const char *wal_path)
{
    /* Exact 006 golden manifest and segment; frame retained from the same
     * unchanged 004 encoding. Pair identity comes from fresh provisioning. */
    static const unsigned char manifest[] =
        "TLINSTAL\1\0\0\0\0\0\0\0\x14\1\0\0\0\0\0\0\1\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xf6\x94\xc8\x24";
    static const unsigned char segment[] =
        "TLSEGMNT\1\0\0\0\0\0\0\0\1\0\0\0\0\0\0\0\x54\0\0\0\xd6\xa4\x16\x17";
    struct timelite_batches db;
    struct timelite_file file = TIMELITE_FILE_INIT;
    struct timelite_record record = {17, UINT64_C(1700000000000000), -123};
    struct timelite_range range = {UINT64_C(1700000000000000), UINT64_MAX, 17, 1};
    unsigned char scratch[TIMELITE_BATCH_SCRATCH], frame[84];
    uint64_t sequence;
    size_t count;
    TEST_CASE("006 golden seek and append order", 0);
    assert(sizeof(manifest) == 65 && sizeof(segment) == 33);
    assert(timelite_batches_init(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_file_open(&file, wal_path) == 0);
    assert(timelite_file_read(&file, 32, frame, 84, &count) == 0 && count == 84);
    assert(timelite_file_truncate(&file, 32) == 0);
    assert(timelite_file_close(&file) == 0);
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_write(&file, 96, manifest, 64, &count) == 0);
    assert(timelite_file_write(&file, 160, segment, 32, &count) == 0);
    assert(timelite_file_write(&file, 192, frame, 84, &count) == 0);
    assert(timelite_file_close(&file) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_seek(&db, record.timestamp_us, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next_range(&db, &range, &record, 1, &count, &sequence,
                                      scratch, sizeof(scratch)) == 0 && sequence == 1 && count == 1);
    record.timestamp_us--;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
    record.timestamp_us++;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_seek(&db, record.timestamp_us, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next(&db, &record, 1, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 1);
    assert(timelite_batches_next(&db, &record, 1, &count, &sequence,
                                scratch, sizeof(scratch)) == 0 && sequence == 2);
    assert(timelite_batches_seek(&db, UINT64_MAX, scratch, sizeof(scratch)) == TIMELITE_END);
    assert(timelite_batches_close(&db) == 0);
    assert(remove_test_file(path) == 0);
    assert(remove_test_file(wal_path) == 0);
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
                                     {UINT32_MAX, UINT64_MAX, INT64_MAX}};
    struct timelite_record output[2];
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    unsigned char saved[240], installed[336], damaged;
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
    assert(timelite_file_size(&file, &size) == 0 && size == 160 + 64 + 208);
    assert(timelite_file_read(&file, 96, installed, sizeof(installed), &transferred) == 0);
    assert(transferred == sizeof(installed));
    assert(memcmp(installed, "TLINSTAL\1\0\0\0\0\0\0\0\xb0\1\0\0\0\0\0\0\2\0\0\0\0\0\0\0", 32) == 0);
    assert(memcmp(installed + 64, "TLSPAN07\1\0\0\0\0\0\0\0\2\0\0\0\0\0\0\0\xd0\0\0\0", 28) == 0);
    assert(memcmp(installed + 128, saved + 32, 208) == 0);
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
    /* Fresh pair with one installed single-record frame (308-byte database).
     * Bit flips: slot 0 (older manifest) still opens; slot 1 and the segment
     * header fail closed at open; the installed frame fails closed at read. */
    assert(timelite_batches_open(&db, path, wal_path, TIMELITE_CREATE_NEW,
                                scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_read(&file, 0, installed, 308, &transferred) == 0 && transferred == 308);
    assert(timelite_file_close(&file) == 0);
    for (prefix = 32; prefix < 308; prefix++)
    {
        TEST_CASE("installed byte flip", prefix);
        assert(timelite_file_open(&file, path) == 0);
        assert(timelite_file_write(&file, 0, installed, 308, &transferred) == 0);
        damaged = (unsigned char)(installed[prefix] ^ 1);
        assert(timelite_file_write(&file, prefix, &damaged, 1, &transferred) == 0);
        assert(timelite_file_close(&file) == 0);
        error = timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                      scratch, sizeof(scratch));
        if (prefix >= 96 && prefix < 224)
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
        if (prefix >= 224)
        {
            struct timelite_batches before = db;
            struct timelite_range range = {0, UINT64_MAX, 0, 0};
            assert(timelite_batches_seek(&db, 0, scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
            assert(timelite_batches_next_range(&db, &range, output, 2, &count, &sequence,
                                              scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
            same_cursor(&db, &before);
            assert(count == 99);
        }
        assert(timelite_batches_close(&db) == 0);
    }
    /* Database cut below data_end with an empty WAL fails closed. */
    for (prefix = 161; prefix < 308; prefix++)
    {
        TEST_CASE("database cut", prefix);
        assert(timelite_file_open(&file, path) == 0);
        assert(timelite_file_write(&file, 0, installed, 308, &transferred) == 0);
        assert(timelite_file_truncate(&file, prefix) == 0);
        assert(timelite_file_close(&file) == 0);
        assert(timelite_batches_open(&db, path, wal_path, TIMELITE_OPEN_EXISTING,
                                    scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
    }
    /* Capacity: a sparse fixture with 16 segments ending 100 bytes below the
     * limit. Checkpoint refuses before any effect; append and close still work. */
    assert(timelite_file_open(&file, path) == 0);
    assert(timelite_file_write(&file, 0, installed, 308, &transferred) == 0);
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
    aggregate_ranges(path, wal_path);
    time_ranges(path, wal_path);
    legacy_range(path, wal_path);
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
