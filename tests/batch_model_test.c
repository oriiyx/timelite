/* Deterministic model, linked instead of the native backend. No test hooks in
 * production. Synced ranges and names survive; unsynced bytes may survive or
 * disappear. Previously synced bytes are never damaged by later writes. */
#include "timelite.h"
#include "file_io.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned char wal_bytes[67108864], wal_stable[67108864];
static unsigned char database_bytes[64], database_stable[64];
struct model_file
{
    unsigned char *bytes, *stable;
    size_t length, stable_length, dirty;
    int exists, stable_exists, owned;
};
static struct model_file files[2];
static int operation, fail_operation, fail_after, error_code;
static size_t transfer_limit;
static int read_failure, size_failure, close_failure, truncate_failure;
static int identity_failure, provisioning_failure, create_failure, race;
static int writes, truncates, provisions, closes;
static unsigned char scratch[TIMELITE_BATCH_SCRATCH];
static struct timelite_record input[64], output[64];

static int index_of(struct timelite_file *file)
{
#if defined(_WIN32)
    return file->handle == &files[0] ? 0 : 1;
#else
    assert(file->fd == 0 || file->fd == 1);
    return file->fd;
#endif
}

static void acquire(struct timelite_file *file, int index)
{
    assert(!files[index].owned);
    files[index].owned = 1;
#if defined(_WIN32)
    file->handle = &files[index];
#else
    file->fd = index;
#endif
}

static void faults_clear(void)
{
    operation = fail_operation = fail_after = 0;
    read_failure = size_failure = close_failure = truncate_failure = 0;
    identity_failure = provisioning_failure = create_failure = race = 0;
    error_code = EIO;
    transfer_limit = SIZE_MAX;
}

static void reset(void)
{
    memset(files, 0, sizeof(files));
    files[0].bytes = database_bytes;
    files[0].stable = database_stable;
    files[1].bytes = wal_bytes;
    files[1].stable = wal_stable;
    faults_clear();
    writes = truncates = provisions = closes = 0;
}

static void persist(int index)
{
    struct model_file *f = &files[index];
    if (f->length > f->dirty)
    {
        memcpy(f->stable + f->dirty, f->bytes + f->dirty, f->length - f->dirty);
    }
    f->stable_length = f->length;
    f->dirty = f->length;
}

static void crash(void)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        struct model_file *f = &files[i];
        memcpy(f->bytes, f->stable, f->stable_length);
        f->length = f->stable_length;
        f->dirty = f->length;
        f->exists = f->stable_exists;
        f->owned = 0;
    }
    faults_clear();
}

int timelite_file_open(struct timelite_file *file, const char *path)
{
    int index = strcmp(path, "db") == 0 ? 0 : 1;
    if (!files[index].exists)
    {
        return ENOENT;
    }
    acquire(file, index);
    return 0;
}

int timelite_file_create(struct timelite_file *file, const char *path)
{
    int index = strcmp(path, "db") == 0 ? 0 : 1;
    if (create_failure == index + 1)
    {
        return EACCES;
    }
    if (race && index == 0)
    {
        files[0].exists = race != 3;
        files[0].length = race == 2 ? 32 : 0;
        if (race == 2)
        {
            files[1].exists = 1;
            files[1].length = 32;
        }
        return EEXIST;
    }
    if (files[index].exists)
    {
        return EEXIST;
    }
    files[index].exists = 1;
    files[index].length = files[index].dirty = 0;
    acquire(file, index);
    return 0;
}

int timelite_file_identity(unsigned char identity[16])
{
    memset(identity, 0x42, 16);
    return identity_failure;
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *count)
{
    struct model_file *f = &files[index_of(file)];
    *count = offset >= f->length ? 0 : f->length - (size_t)offset;
    if (*count > length)
    {
        *count = length;
    }
    if (read_failure == 2 && *count != 0)
    {
        (*count)--;
    }
    if (*count != 0)
    {
        memcpy(buffer, f->bytes + (size_t)offset, *count);
    }
    return read_failure == 1 ? EIO : 0;
}

int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *count)
{
    int index = index_of(file);
    struct model_file *f = &files[index];
    operation++;
    writes++;
    *count = 0;
    if (operation == fail_operation && !fail_after)
    {
        return error_code;
    }
    *count = length < transfer_limit ? length : transfer_limit;
    assert(offset + *count <= (index ? sizeof(wal_bytes) : sizeof(database_bytes)));
    memcpy(f->bytes + (size_t)offset, buffer, *count);
    if (offset < f->dirty)
    {
        f->dirty = (size_t)offset;
    }
    if (offset + *count > f->length)
    {
        f->length = (size_t)offset + *count;
    }
    return operation == fail_operation ? error_code : 0;
}

int timelite_file_sync(struct timelite_file *file)
{
    operation++;
    if (operation == fail_operation && !fail_after)
    {
        return error_code;
    }
    persist(index_of(file));
    return operation == fail_operation ? error_code : 0;
}

int timelite_file_provision(struct timelite_file *file, const char *path)
{
    int index = index_of(file);
    (void)path;
    provisions++;
    if (provisioning_failure == index + 1)
    {
        return EIO;
    }
    persist(index);
    files[index].stable_exists = 1;
    return 0;
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    if (size_failure)
    {
        return EIO;
    }
    *size = files[index_of(file)].length;
    return 0;
}

int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    struct model_file *f = &files[index_of(file)];
    truncates++;
    if (truncate_failure == 1)
    {
        return EIO;
    }
    assert(size <= f->length);
    f->length = (size_t)size;
    if (size < f->dirty)
    {
        f->dirty = (size_t)size;
    }
    return truncate_failure == 2 ? EIO : 0;
}

int timelite_file_close(struct timelite_file *file)
{
    files[index_of(file)].owned = 0;
#if defined(_WIN32)
    file->handle = NULL;
#else
    file->fd = -1;
#endif
    closes++;
    return close_failure ? EACCES : 0;
}

static int open_db(struct timelite_batches *db, enum timelite_open_mode mode)
{
    return timelite_batches_open(db, "db", "wal", mode, scratch, sizeof(scratch));
}

static void new_db(struct timelite_batches *db)
{
    reset();
    assert(timelite_batches_init(db) == 0);
    assert(open_db(db, TIMELITE_CREATE_NEW) == 0);
    operation = 0;
}

static void check_batches(struct timelite_batches *db, size_t expected)
{
    size_t i, count = 99;
    uint64_t sequence = 99;
    assert(timelite_batches_rewind(db) == 0);
    for (i = 0; i < expected; i++)
    {
        assert(timelite_batches_next(db, output, 64, &count, &sequence,
                                    scratch, sizeof(scratch)) == 0);
        assert(sequence == i + 1 && count == 1);
        assert(output[0].series == input[0].series &&
               output[0].value == input[0].value &&
               output[0].timestamp_us == input[0].timestamp_us);
    }
    count = 99;
    sequence = 99;
    assert(timelite_batches_next(db, output, 64, &count, &sequence,
                                scratch, sizeof(scratch)) == TIMELITE_END);
    assert(sequence == 99 && count == 99);
}

static void interruptions(void)
{
    struct timelite_batches db;
    uint64_t sequence;
    int boundary, after;
    size_t prefix;
    /* All body-write/body-sync/commit-write/commit-sync error boundaries,
     * before and after effects. Persisted commit can appear after lost success. */
    for (boundary = 1; boundary <= 4; boundary++)
    {
        for (after = 0; after <= 1; after++)
        {
            new_db(&db);
            assert(timelite_batches_append(&db, input, 1, scratch,
                                          sizeof(scratch), &sequence) == 0);
            operation = 0;
            fail_operation = boundary;
            fail_after = after;
            sequence = 99;
            assert(timelite_batches_append(&db, input, 1, scratch,
                                          sizeof(scratch), &sequence) == EIO);
            assert(sequence == 99);
            assert(timelite_batches_append(&db, input, 1, scratch,
                                          sizeof(scratch), &sequence) == TIMELITE_RECOVERY_REQUIRED);
            assert(timelite_batches_rewind(&db) == TIMELITE_RECOVERY_REQUIRED);
            crash();
            assert(timelite_batches_init(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
            check_batches(&db, boundary == 4 && after ? 2 : 1);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    /* Every short body write, including zero progress and short-success results.
     * Allow unsynced bytes to reach stable storage before the interruption. */
    for (prefix = 0; prefix < 52; prefix++)
    {
        new_db(&db);
        transfer_limit = prefix;
        assert(timelite_batches_append(&db, input, 1, scratch,
                                      sizeof(scratch), &sequence) == EIO);
        persist(1);
        crash();
        assert(timelite_batches_init(&db) == 0);
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) ==
               (prefix > 0 && prefix < 32 ? TIMELITE_INVALID_DATABASE : 0));
        if (prefix == 0 || prefix >= 32)
        {
            check_batches(&db, 0);
            assert(timelite_batches_append(&db, input, 1, scratch,
                                          sizeof(scratch), &sequence) == 0 && sequence == 1);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    /* Acknowledged append survives a crash without close. */
    new_db(&db);
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    crash();
    assert(timelite_batches_init(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    check_batches(&db, 1);
    assert(timelite_batches_close(&db) == 0);
}

static void failures(void)
{
    struct timelite_batches db;
    uint64_t sequence = 99;
    size_t count = 99;
    int failure, error, previous;
    for (failure = 1; failure <= 8; failure++)
    {
        new_db(&db);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(timelite_batches_close(&db) == 0);
        previous = writes;
        if (failure <= 2)
        {
            read_failure = failure;
        }
        if (failure == 3)
        {
            size_failure = 1;
        }
        if (failure == 4 || failure == 5)
        {
            provisioning_failure = failure - 3;
        }
        if (failure >= 6)
        {
            wal_bytes[failure == 6 ? 12 : failure == 7 ? 70 : 100] ^= 1;
        }
        close_failure = 1;
        error = open_db(&db, TIMELITE_OPEN_EXISTING);
        assert(error == (failure == 2 || failure >= 6 ? TIMELITE_INVALID_DATABASE : EIO));
        assert(writes == previous && !files[0].owned && !files[1].owned);
        assert(timelite_batches_close(&db) == EBADF);
    }
    new_db(&db);
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    output[0].series = 1234;
    read_failure = 1;
    assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                scratch, sizeof(scratch)) == EIO);
    assert(count == 99 && output[0].series == 1234);
    read_failure = 0;
    assert(timelite_batches_next(&db, output, 0, &count, &sequence,
                                scratch, sizeof(scratch)) == TIMELITE_BUFFER_TOO_SMALL);
    assert(count == 99 && output[0].series == 1234);
    check_batches(&db, 1);
    close_failure = 1;
    previous = closes;
    assert(timelite_batches_close(&db) == EACCES && closes == previous + 2);
    assert(timelite_batches_close(&db) == EBADF);
    faults_clear();
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(timelite_batches_close(&db) == 0);
    /* Interrupted tail cleanup: truncate before/after effects and final sync. */
    for (failure = 1; failure <= 4; failure++)
    {
        new_db(&db);
        fail_operation = 3;
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == EIO);
        crash();
        assert(timelite_batches_init(&db) == 0);
        if (failure <= 2)
        {
            truncate_failure = failure;
        }
        else
        {
            fail_operation = 1;
            fail_after = failure == 4;
        }
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == EIO);
        crash();
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
        check_batches(&db, 0);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(timelite_batches_close(&db) == 0);
    }
}

static void creation(void)
{
    struct timelite_batches db;
    int i;
    for (i = 1; i <= 8; i++)
    {
        reset();
        assert(timelite_batches_init(&db) == 0);
        if (i == 1)
        {
            identity_failure = EIO;
        }
        if (i == 2 || i == 3)
        {
            create_failure = i - 1;
        }
        if (i == 4 || i == 5)
        {
            fail_operation = i - 3;
        }
        if (i == 6 || i == 7)
        {
            provisioning_failure = i - 5;
        }
        if (i == 8)
        {
            race = 1;
        }
        close_failure = 1;
        assert(open_db(&db, TIMELITE_OPEN_OR_CREATE) != 0);
        assert(!files[0].owned && !files[1].owned);
        if (i == 1)
        {
            assert(!files[0].exists && !files[1].exists);
        }
    }
    /* A complete WAL header may remain after its write reports failure. */
    reset();
    assert(timelite_batches_init(&db) == 0);
    fail_operation = 2;
    fail_after = 1;
    close_failure = 1;
    assert(open_db(&db, TIMELITE_CREATE_NEW) == EIO);
    faults_clear();
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(timelite_batches_close(&db) == 0);
    /* Race winner supplies a complete pair, or disappears before bounded open. */
    for (i = 2; i <= 3; i++)
    {
        new_db(&db);
        assert(timelite_batches_close(&db) == 0);
        reset(); /* Retain fixture bytes, but remove modeled names. */
        race = i;
        assert(open_db(&db, TIMELITE_OPEN_OR_CREATE) == (i == 2 ? 0 : ENOENT));
        assert(writes == 0);
        if (i == 2)
        {
            assert(timelite_batches_close(&db) == 0);
        }
    }
    reset();
    files[1].exists = 1;
    assert(timelite_batches_init(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_OR_CREATE) == TIMELITE_PAIR_MISMATCH);
    assert(!files[0].exists && writes == 0);
    new_db(&db);
    assert(timelite_batches_close(&db) == 0);
    files[1].exists = 0;
    assert(open_db(&db, TIMELITE_OPEN_OR_CREATE) == ENOENT);
    assert(writes == 2);
}

/* Independent fixture CRC computation used to construct well-framed damage. */
static void fixture_crc(unsigned char *bytes, size_t length)
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
    crc = ~crc;
    for (i = 0; i < 4; i++)
    {
        bytes[length + i] = (unsigned char)(crc >> (i * 8));
    }
}

static void format_cases(void)
{
    struct timelite_batches db;
    uint64_t sequence;
    int i, expected, before;
    size_t prefix;
    /* Golden bytes generated independently with Python struct + zlib.crc32. */
    static const unsigned char expected_frame[] =
        "\x54\x4c\x42\x41\x54\x43\x48\x21\x01\x00\x00\x00\x00\x00\x00\x00"
        "\x01\x00\x00\x00\x34\x00\x00\x00\x00\x00\x00\x00\x99\x96\x15\xf9"
        "\x11\x00\x00\x00\x00\x40\x1e\x18\x24\x0a\x06\x00\x85\xff\xff\xff"
        "\xff\xff\xff\xff\x54\x4c\x43\x4f\x4d\x4d\x49\x54\x01\x00\x00\x00"
        "\x00\x00\x00\x00\x01\x00\x00\x00\x34\x00\x00\x00\x0c\x78\xc6\x22"
        "\xbb\xf6\xda\x72";
    for (i = 0; i < 8; i++)
    {
        new_db(&db);
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(sizeof(expected_frame) - 1 == 84);
        assert(memcmp(wal_bytes + 32, expected_frame, 84) == 0);
        assert(timelite_batches_close(&db) == 0);
        expected = TIMELITE_INVALID_DATABASE;
        if (i == 0)
        {
            wal_bytes[12] ^= 1;
            fixture_crc(wal_bytes, 28);
            expected = TIMELITE_PAIR_MISMATCH;
        }
        if (i == 1)
        {
            wal_bytes[8] = 3;
            expected = TIMELITE_UNSUPPORTED_VERSION;
        }
        if (i == 2)
        {
            wal_bytes[40] = 2; /* Sequence gap with valid header CRC. */
            fixture_crc(wal_bytes + 32, 28);
        }
        if (i == 3 || i == 4)
        {
            wal_bytes[48] = i == 3 ? 0 : 65;
            fixture_crc(wal_bytes + 32, 28);
        }
        if (i == 5)
        {
            memset(wal_bytes + 52, 255, 4);
            fixture_crc(wal_bytes + 32, 28);
        }
        if (i == 6)
        {
            wal_bytes[92] = 2; /* Malformed commit with valid commit CRC. */
            fixture_crc(wal_bytes + 84, 28);
        }
        if (i == 7)
        {
            database_bytes[8] = 1;
            expected = TIMELITE_UNSUPPORTED_VERSION;
        }
        before = provisions;
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == expected);
        assert(provisions == before && truncates == 0);
    }
    /* All incomplete creation-header prefixes; artifacts remain untouched. */
    for (prefix = 0; prefix < 32; prefix++)
    {
        reset();
        assert(timelite_batches_init(&db) == 0);
        transfer_limit = prefix;
        assert(open_db(&db, TIMELITE_CREATE_NEW) == EIO);
        assert(files[0].length == prefix && files[1].length == 0);
        faults_clear();
        before = writes;
        assert(open_db(&db, TIMELITE_OPEN_OR_CREATE) == TIMELITE_INVALID_DATABASE);
        assert(writes == before);
    }
}

static void capacity(void)
{
    struct timelite_batches db;
    uint64_t sequence = 0, expected = 0;
    size_t saved_length;
    unsigned char last[1344];
    int error, previous;
    new_db(&db);
    do
    {
        error = timelite_batches_append(&db, input, 64, scratch, sizeof(scratch), &sequence);
        if (error == 0)
        {
            expected++;
            assert(sequence == expected);
        }
    } while (error == 0);
    assert(error == TIMELITE_WAL_FULL);
    saved_length = files[1].length;
    memcpy(last, wal_bytes + saved_length - sizeof(last), sizeof(last));
    previous = writes;
    assert(timelite_batches_append(&db, input, 64, scratch, sizeof(scratch), &sequence) == TIMELITE_WAL_FULL);
    assert(files[1].length == saved_length && writes == previous);
    assert(memcmp(last, wal_bytes + saved_length - sizeof(last), sizeof(last)) == 0);
    assert(timelite_batches_close(&db) == 0);
    crash();
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_sequence == expected);
    assert(timelite_batches_close(&db) == 0);
}

int main(void)
{
    input[0].series = 17;
    input[0].timestamp_us = UINT64_C(1700000000000000);
    input[0].value = -123;
    interruptions();
    failures();
    creation();
    format_cases();
    capacity();
    puts("batch persisted/volatile model, failures, recovery and 64 MiB capacity: passed");
    return 0;
}
