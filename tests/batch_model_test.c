/* Deterministic model, linked instead of the native backend. No test hooks in
 * production. Synced ranges and names survive; unsynced bytes may survive or
 * disappear. Previously synced bytes are never damaged by later writes. */
#include "timelite.h"
#include "file_io.h"
#include "test_assert.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The database must hold one full-WAL checkpoint plus headers and a little more. */
#define MODEL_DATABASE (67108864 + 65536)
static unsigned char wal_bytes[67108864], wal_stable[67108864];
static unsigned char database_bytes[MODEL_DATABASE], database_stable[MODEL_DATABASE];
struct model_file
{
    unsigned char *bytes, *stable;
    size_t length, stable_length, dirty;
    int exists, stable_exists, owned;
};
static struct model_file files[2];
static int status_checking, aggregate_checking;
static int operation, fail_operation, fail_after, error_code;
static size_t transfer_limit;
static int read_failure, size_failure, close_failure, truncate_failure;
static int identity_failure, provisioning_failure, provision_error, create_failure, race;
static int writes, truncates, provisions, closes;
static int read_calls, fail_read_at, payload_reads, short_operation;
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
    read_calls = fail_read_at = payload_reads = short_operation = 0;
    read_failure = size_failure = close_failure = truncate_failure = 0;
    identity_failure = provisioning_failure = create_failure = race = 0;
    error_code = provision_error = EIO;
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
        TEST_CASE(__func__, i);
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
    assert(!status_checking);
    assert(!aggregate_checking);
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
    assert(!status_checking);
    assert(!aggregate_checking);
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
    assert(!status_checking);
    assert(!aggregate_checking);
    memset(identity, 0x42, 16);
    return identity_failure;
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *count)
{
    assert(!status_checking);
    struct model_file *f = &files[index_of(file)];
    read_calls++;
    if (length > 32)
    {
        payload_reads++;
    }
    if (fail_read_at == read_calls)
    {
        return EIO;
    }
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
    assert(!status_checking);
    assert(!aggregate_checking);
    int index = index_of(file);
    struct model_file *f = &files[index];
    operation++;
    writes++;
    *count = 0;
    if (operation == fail_operation && !fail_after)
    {
        return error_code;
    }
    *count = (short_operation == 0 || short_operation == operation) && length > transfer_limit ?
             transfer_limit : length;
    assert(offset + *count <= (index ? sizeof(wal_bytes) : sizeof(database_bytes)));
    if (offset > f->length)
    {
        /* Writing past EOF zero-fills the gap, as both native backends do. */
        memset(f->bytes + f->length, 0, (size_t)offset - f->length);
    }
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
    assert(!status_checking);
    assert(!aggregate_checking);
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
    assert(!status_checking);
    assert(!aggregate_checking);
    int index = index_of(file);
    (void)path;
    provisions++;
    if (provisioning_failure == index + 1)
    {
        return provision_error;
    }
    persist(index);
    files[index].stable_exists = 1;
    return 0;
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    assert(!status_checking);
    assert(!aggregate_checking);
    if (size_failure)
    {
        return EIO;
    }
    *size = files[index_of(file)].length;
    return 0;
}

int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    assert(!status_checking);
    assert(!aggregate_checking);
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
    assert(!status_checking);
    assert(!aggregate_checking);
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
        TEST_CASE(__func__, i);
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

/* Counts batches of any size, asserting sequence continuity from 1. */
static uint64_t count_batches(struct timelite_batches *db)
{
    size_t count;
    uint64_t sequence, total = 0;
    int error;
    assert(timelite_batches_rewind(db) == 0);
    while ((error = timelite_batches_next(db, output, 64, &count, &sequence,
                                          scratch, sizeof(scratch))) == 0)
    {
        total++;
        assert(sequence == total && count >= 1);
    }
    assert(error == TIMELITE_END);
    return total;
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
        TEST_CASE(__func__, boundary);
        for (after = 0; after <= 1; after++)
        {
            TEST_CASE(after ? "append after effect" : "append before effect", boundary);
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
        TEST_CASE(__func__, prefix);
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
        TEST_CASE(__func__, failure);
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
        TEST_CASE(__func__, failure);
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
        TEST_CASE(__func__, i);
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
        TEST_CASE(__func__, i);
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
static uint32_t fixture_crc_value(const unsigned char *bytes, size_t length)
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

static void encode_fixture32(unsigned char *bytes, uint32_t value)
{
    size_t i;
    for (i = 0; i < 4; i++)
    {
        bytes[i] = (unsigned char)(value >> (i * 8));
    }
}

static void fixture_crc(unsigned char *bytes, size_t length)
{
    uint32_t crc = fixture_crc_value(bytes, length);
    size_t i;
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
        TEST_CASE(__func__, i);
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
    /* All incomplete creation-image prefixes; artifacts remain untouched. A
     * complete header with torn manifest slots is an empty database, but the
     * empty WAL created just before fails validation; nothing is repaired. */
    for (prefix = 0; prefix < 160; prefix++)
    {
        TEST_CASE(__func__, prefix);
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

/* Golden install bytes generated independently with Python struct + zlib.crc32. */
static const unsigned char golden_manifest0[] =
    "\x54\x4c\x49\x4e\x53\x54\x41\x4c\x00\x00\x00\x00\x00\x00\x00\x00"
    "\xa0\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x24\x40\xb6\x73";
static const unsigned char golden_manifest1[] =
    "\x54\x4c\x49\x4e\x53\x54\x41\x4c\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x34\x01\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x40\x1e\x18\x24\x0a\x06\x00\xa0\x00\x00\x00\x00\x00\x00\x00"
    "\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x0a\x6c\x19\x7a";
static const unsigned char golden_segment[] =
    "\x54\x4c\x53\x50\x41\x4e\x30\x37\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x00\x00\x00\x00\x54\x00\x00\x00\x00\x40\x1e\x18"
    "\x24\x0a\x06\x00\x00\x40\x1e\x18\x24\x0a\x06\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x65\x6a\x73\xde";
static const unsigned char golden_unordered[] =
    "\x54\x4c\x55\x4e\x4f\x52\x30\x37\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00\x00\x00\x00\x00\x00\x00\xa8\x00\x00\x00\x14\x00\x00\x00"
    "\x00\x00\x00\x00\x64\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x33\xb7\x2c\x4c";
static const unsigned char zero_slot[64];

typedef char golden_sizes_include_terminators[
    sizeof(golden_manifest0) == 65 && sizeof(golden_manifest1) == 65 &&
    sizeof(golden_segment) == 65 ? 1 : -1];

static int checkpoint(struct timelite_batches *db)
{
    return timelite_batches_checkpoint(db, scratch, sizeof(scratch));
}

static void append_one(struct timelite_batches *db, uint64_t expected)
{
    uint64_t sequence = 99;
    assert(timelite_batches_append(db, input, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(sequence == expected);
}

static void status_values(struct timelite_batches *db, uint64_t committed,
                          uint64_t installed, uint64_t segments,
                          uint64_t wal, uint64_t bytes, uint64_t timestamp)
{
    struct timelite_batches_status status;
    unsigned char before[sizeof(*db)];
    memcpy(before, db, sizeof(*db));
    status_checking = 1;
    assert(timelite_batches_get_status(db, &status) == 0);
    status_checking = 0;
    assert(status.committed_batches == committed);
    assert(status.installed_batches == installed);
    assert(status.pending_batches == committed - installed);
    assert(status.installed_segments == segments);
    assert(status.wal_bytes == wal && status.installed_bytes == bytes);
    assert(status.last_timestamp_us == timestamp);
    assert(memcmp(before, db, sizeof(*db)) == 0);
}

static void status_errors(struct timelite_batches *db, int expected)
{
    struct timelite_batches_status status;
    unsigned char before[sizeof(status)], handle[sizeof(*db)];
    memset(&status, 0xa5, sizeof(status));
    memcpy(before, &status, sizeof(status));
    memcpy(handle, db, sizeof(*db));
    status_checking = 1;
    assert(timelite_batches_get_status(NULL, &status) == EINVAL);
    assert(timelite_batches_get_status(NULL, NULL) == EINVAL);
    assert(timelite_batches_get_status(db, NULL) == EINVAL);
    assert(timelite_batches_get_status(db, &status) == expected);
    status_checking = 0;
    assert(memcmp(before, &status, sizeof(status)) == 0);
    assert(memcmp(handle, db, sizeof(*db)) == 0);
}

static void database_status(void)
{
    struct timelite_batches db;
    uint64_t saved_timestamp = input[0].timestamp_us;
    uint64_t sequence;
    size_t count;
    TEST_CASE("metadata status lifecycle and no storage operations", 0);
    new_db(&db);
    status_values(&db, 0, 0, 0, 32, 0, 0);
    input[0].timestamp_us = 0;
    append_one(&db, 1);
    status_values(&db, 1, 0, 0, 116, 0, 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                scratch, sizeof(scratch)) == 0);
    status_values(&db, 1, 0, 0, 116, 0, 0);
    assert(checkpoint(&db) == 0);
    status_values(&db, 1, 1, 1, 32, 148, 0);
    input[0].timestamp_us = 42;
    append_one(&db, 2);
    status_values(&db, 2, 1, 1, 116, 148, 42);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                scratch, sizeof(scratch)) == 0);
    assert(sequence == 2);
    assert(timelite_batches_close(&db) == 0);
    status_errors(&db, EBADF);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    status_values(&db, 2, 1, 1, 116, 148, 42);
    assert(checkpoint(&db) == 0);
    status_values(&db, 2, 2, 2, 32, 296, 42);
    operation = 0;
    fail_operation = 1;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch),
                                  &sequence) == EIO);
    status_errors(&db, TIMELITE_RECOVERY_REQUIRED);
    faults_clear();
    assert(timelite_batches_close(&db) == 0);
    input[0].timestamp_us = saved_timestamp;
}

static void checkpoint_basic(void)
{
    struct timelite_batches db;
    uint64_t sequence = 99;
    size_t count = 99;
    int previous;
    /* Creation writes the 160-byte image: header, generation 0, zero slot. */
    new_db(&db);
    assert(files[0].length == 160 && memcmp(database_bytes + 32, golden_manifest0, 64) == 0);
    assert(memcmp(database_bytes + 96, zero_slot, 64) == 0);
    /* Argument checks and the empty WAL have no effect. */
    previous = writes;
    assert(timelite_batches_checkpoint(&db, NULL, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_checkpoint(&db, scratch, 1343) == TIMELITE_BUFFER_TOO_SMALL);
    assert(checkpoint(&db) == 0 && writes == previous && db.private_generation == 0);
    assert(timelite_batches_checkpoint(NULL, scratch, sizeof(scratch)) == EINVAL);
    /* One golden frame installed: segment at 160, frame at 224, manifest in slot 1. */
    append_one(&db, 1);
    assert(checkpoint(&db) == 0);
    assert(files[0].length == 308 && files[1].length == 32 && files[1].stable_length == 32);
    assert(memcmp(database_bytes + 160, golden_segment, 64) == 0);
    assert(memcmp(database_bytes + 224, wal_stable + 32, 84) == 0);
    assert(memcmp(database_bytes + 96, golden_manifest1, 64) == 0);
    assert(memcmp(database_bytes + 32, golden_manifest0, 64) == 0);
    assert(db.private_installed == 1 && db.private_generation == 1 && db.private_data_end == 308);
    check_batches(&db, 1);
    /* Sequences continue in the reclaimed WAL; reads combine both files. */
    append_one(&db, 2);
    append_one(&db, 3);
    assert(files[1].length == 32 + 2 * 84 && wal_bytes[40] == 2);
    check_batches(&db, 3);
    /* The read position survives a checkpoint, including at END. */
    assert(timelite_batches_rewind(&db) == 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
    assert(sequence == 1);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
    assert(sequence == 2);
    assert(checkpoint(&db) == 0);
    assert(db.private_generation == 2 && db.private_installed == 3 && files[1].length == 32);
    assert(memcmp(database_bytes + 96, golden_manifest1, 64) == 0);
    assert(database_bytes[40] == 2 && memcmp(database_bytes + 32, "TLINSTAL", 8) == 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
    assert(sequence == 3);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_END);
    append_one(&db, 4);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
    assert(sequence == 4);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_END);
    /* Small output leaves the position at a segment boundary unchanged. */
    assert(timelite_batches_rewind(&db) == 0);
    assert(timelite_batches_next(&db, output, 0, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_BUFFER_TOO_SMALL);
    check_batches(&db, 4);
    /* Crash without close, reopen: three segments and an empty WAL. */
    crash();
    assert(timelite_batches_init(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_generation == 3 && db.private_installed == 4 && db.private_end == 32);
    check_batches(&db, 4);
    /* Feature 004 recovery still applies to appends after reclaim. */
    append_one(&db, 5);
    operation = 0;
    fail_operation = 3;
    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == EIO);
    crash();
    assert(timelite_batches_init(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(files[1].length == 32 + 84);
    check_batches(&db, 5);
    append_one(&db, 6);
    check_batches(&db, 6);
    assert(timelite_batches_close(&db) == 0);
    /* Windows contract: provisioning ENOTSUP fails open, checkpoint is EBADF. */
    reset();
    assert(timelite_batches_init(&db) == 0);
    provisioning_failure = 1;
    provision_error = ENOTSUP;
    assert(open_db(&db, TIMELITE_CREATE_NEW) == ENOTSUP);
    previous = writes;
    assert(checkpoint(&db) == EBADF && writes == previous);
    assert(!files[0].owned && !files[1].owned);
}

/* Every write, sync and truncate boundary of a checkpoint, before and after
 * its effect, optionally with all unsynced bytes surviving the crash. The
 * first loop starts from a database with one installed segment so a later
 * failure can never invalidate it; the second starts from the feature 004
 * 32-byte layout and covers the slot upgrade. */
static void checkpoint_boundaries(void)
{
    struct timelite_batches db;
    uint64_t sequence = 99;
    int legacy, boundary, after, survive, durable, operations;
    for (legacy = 0; legacy <= 1; legacy++)
    {
        /* Regular: header, 2 frames, sync, manifest, sync, [truncate], sync.
         * Legacy: upgrade write and sync, header, 3 frames, sync, manifest,
         * sync, [truncate], sync. Boundaries past the count are truncate
         * failures before and after effect. */
        operations = legacy ? 10 : 7;
        for (boundary = 1; boundary <= operations + 2; boundary++)
        {
            for (after = 0; after <= 1; after++)
            {
                for (survive = 0; survive <= 1; survive++)
                {
                    TEST_CASE(legacy ? "legacy checkpoint boundary" : "checkpoint boundary",
                              boundary * 100 + after * 10 + survive);
                    new_db(&db);
                    append_one(&db, 1);
                    if (legacy)
                    {
                        assert(timelite_batches_close(&db) == 0);
                        files[0].length = 32;
                        persist(0);
                        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                        assert(db.private_legacy == 1 && db.private_data_end == 160);
                    }
                    else
                    {
                        assert(checkpoint(&db) == 0);
                    }
                    append_one(&db, 2);
                    append_one(&db, 3);
                    operation = 0;
                    if (boundary <= operations)
                    {
                        fail_operation = boundary;
                        fail_after = after;
                    }
                    else
                    {
                        truncate_failure = boundary - operations;
                    }
                    assert(checkpoint(&db) == EIO);
                    assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch),
                                                  &sequence) == TIMELITE_RECOVERY_REQUIRED);
                    assert(checkpoint(&db) == TIMELITE_RECOVERY_REQUIRED);
                    assert(timelite_batches_rewind(&db) == TIMELITE_RECOVERY_REQUIRED);
                    if (survive)
                    {
                        persist(0);
                        persist(1);
                    }
                    crash();
                    assert(timelite_batches_init(&db) == 0);
                    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                    /* The install is durable exactly when its manifest reached
                     * stable storage; the WAL is then finished off at open. */
                    durable = boundary > operations - 1 ||
                              (boundary == operations - 1 && (after || survive)) ||
                              (boundary == operations - 2 && after && survive);
                    if (durable)
                    {
                        assert(db.private_installed == 3 && files[1].length == 32);
                        assert(db.private_generation == (legacy ? 1 : 2));
                    }
                    else
                    {
                        assert(db.private_installed == (legacy ? 0 : 1));
                        assert(files[1].length == 32 + 84 * (legacy ? 3 : 2));
                        assert(db.private_generation == (legacy ? 0 : 1));
                    }
                    check_batches(&db, 3);
                    append_one(&db, 4);
                    check_batches(&db, 4);
                    assert(checkpoint(&db) == 0);
                    assert(db.private_legacy == 0 && files[1].length == 32);
                    assert(db.private_installed == 4);
                    check_batches(&db, 4);
                    crash();
                    assert(timelite_batches_init(&db) == 0);
                    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                    check_batches(&db, 4);
                    assert(timelite_batches_close(&db) == 0);
                }
            }
        }
    }
}

/* Builds a stale WAL: two committed frames installed, reclaim interrupted. */
static void stale_wal(struct timelite_batches *db)
{
    new_db(db);
    append_one(db, 1);
    append_one(db, 2);
    truncate_failure = 1;
    assert(checkpoint(db) == EIO);
    crash();
    assert(files[1].length == 200 && files[0].length == 160 + 64 + 168);
    assert(timelite_batches_init(db) == 0);
}

static void checkpoint_format(void)
{
    struct timelite_batches db;
    uint64_t sequence = 99;
    size_t count = 99;
    int i, before, before_truncates, error;
    /* Damage after one or two checkpoints. Cases 0..5 must fail closed; 6
     * (older manifest damaged) opens on the newest manifest. */
    for (i = 0; i <= 6; i++)
    {
        TEST_CASE(__func__, i);
        new_db(&db);
        append_one(&db, 1);
        assert(checkpoint(&db) == 0);
        if (i == 6)
        {
            append_one(&db, 2);
            assert(checkpoint(&db) == 0);
        }
        assert(timelite_batches_close(&db) == 0);
        if (i == 0)
        {
            database_bytes[100] ^= 1; /* Newest manifest damaged after reclaim. */
        }
        if (i == 1)
        {
            database_bytes[148] = 1; /* Nonzero reserved byte with valid CRC. */
            fixture_crc(database_bytes + 96, 60);
        }
        if (i == 2)
        {
            database_bytes[112] = 0x35; /* data_end 309 beyond the 308-byte file. */
            fixture_crc(database_bytes + 96, 60);
        }
        if (i == 3)
        {
            memcpy(database_bytes + 32, database_bytes + 96, 64); /* Generation 1 in slot 0. */
            memset(database_bytes + 96, 0, 64);
        }
        if (i == 4)
        {
            database_bytes[168] = 2; /* Segment first sequence with valid CRC. */
            fixture_crc(database_bytes + 160, 60);
        }
        if (i == 5)
        {
            database_bytes[184] = 85; /* Segment body length with valid CRC. */
            fixture_crc(database_bytes + 160, 60);
        }
        if (i == 6)
        {
            database_bytes[100] ^= 1; /* Older manifest (generation 1) damaged. */
        }
        before = writes;
        before_truncates = truncates;
        error = open_db(&db, TIMELITE_OPEN_EXISTING);
        assert(error == (i == 6 ? 0 : TIMELITE_INVALID_DATABASE));
        assert(writes == before && truncates == before_truncates);
        if (i == 6)
        {
            check_batches(&db, 2);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    /* Newest manifest damaged after appends resumed: sequence gap fails closed. */
    new_db(&db);
    append_one(&db, 1);
    assert(checkpoint(&db) == 0);
    append_one(&db, 2);
    assert(timelite_batches_close(&db) == 0);
    database_bytes[100] ^= 1;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == TIMELITE_INVALID_DATABASE);
    /* Every single-bit flip in the segment header or manifest fails closed;
     * a flip inside the installed frame opens and then fails at that read. */
    for (i = 96; i < 308; i++)
    {
        TEST_CASE("installed byte flip", i);
        new_db(&db);
        append_one(&db, 1);
        assert(checkpoint(&db) == 0);
        append_one(&db, 2);
        assert(timelite_batches_close(&db) == 0);
        database_bytes[i] ^= 1;
        error = open_db(&db, TIMELITE_OPEN_EXISTING);
        if (i < 224)
        {
            assert(error == TIMELITE_INVALID_DATABASE);
            continue;
        }
        assert(error == 0);
        output[0].series = 1234;
        assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch,
                                    sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
        assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch,
                                    sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
        assert(count == 99 && output[0].series == 1234 && db.private_cursor == 160);
        assert(timelite_batches_close(&db) == 0);
    }
    /* Stale WAL: finished at open; damaged or extended stale WALs fail closed. */
    stale_wal(&db);
    before_truncates = truncates;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(truncates == before_truncates + 1 && files[1].length == 32);
    assert(files[1].stable_length == 32);
    assert(db.private_installed == 2 && db.private_sequence == 2);
    check_batches(&db, 2);
    append_one(&db, 3);
    check_batches(&db, 3);
    assert(timelite_batches_close(&db) == 0);
    stale_wal(&db);
    wal_bytes[100] ^= 1;
    persist(1);
    before_truncates = truncates;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == TIMELITE_INVALID_DATABASE);
    assert(truncates == before_truncates);
    stale_wal(&db);
    memcpy(wal_bytes + 200, wal_bytes + 116, 84); /* Frame 3 follows installed 1..2. */
    wal_bytes[208] = 3;
    fixture_crc(wal_bytes + 200, 28);
    wal_bytes[260] = 3;
    encode_fixture32(wal_bytes + 276, fixture_crc_value(wal_bytes + 200, 52));
    fixture_crc(wal_bytes + 252, 28);
    files[1].length = 284;
    persist(1);
    before_truncates = truncates;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == TIMELITE_INVALID_DATABASE);
    assert(truncates == before_truncates);
    /* Stale WAL with an incomplete tail is impossible under the protocol. */
    stale_wal(&db);
    files[1].length = 190;
    persist(1);
    before_truncates = truncates;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == TIMELITE_INVALID_DATABASE);
    assert(truncates == before_truncates);
    /* Database cut below data_end with an empty WAL: orphan bytes without
     * uninstalled WAL frames fail closed (a cut to exactly 160 or below is
     * indistinguishable from a fresh database and is outside the model). */
    for (i = 161; i < 308; i++)
    {
        TEST_CASE("database cut", i);
        new_db(&db);
        append_one(&db, 1);
        assert(checkpoint(&db) == 0);
        assert(timelite_batches_close(&db) == 0);
        files[0].length = (size_t)i;
        persist(0);
        before = writes;
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == TIMELITE_INVALID_DATABASE);
        assert(writes == before);
    }
    /* Torn creation image: header complete, slots partial, then a WAL exists. */
    new_db(&db);
    assert(timelite_batches_close(&db) == 0);
    files[0].length = 100;
    persist(0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_legacy == 0 && db.private_generation == 0);
    append_one(&db, 1);
    assert(checkpoint(&db) == 0);
    check_batches(&db, 1);
    assert(timelite_batches_close(&db) == 0);
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
    /* Checkpoint the full WAL, then append past the old 64 MiB limit. */
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(files[1].length == 32 && files[0].length == 192 + saved_length);
    assert(db.private_installed == expected && db.private_generation == 1);
    assert(memcmp(database_bytes + 224, wal_stable + 32, saved_length - 32) == 0);
    assert(timelite_batches_append(&db, input, 64, scratch, sizeof(scratch), &sequence) == 0);
    assert(sequence == expected + 1);
    assert(count_batches(&db) == expected + 1);
    crash();
    assert(timelite_batches_init(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(count_batches(&db) == expected + 1);
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
    assert(db.private_generation == 2 && files[1].length == 32);
    assert(count_batches(&db) == expected + 1);
    assert(timelite_batches_close(&db) == 0);
}

static void assert_cursor(const struct timelite_batches *db, const struct timelite_batches *before)
{
    assert(db->private_cursor == before->private_cursor);
    assert(db->private_segment_end == before->private_segment_end);
    assert(db->private_read_sequence == before->private_read_sequence);
    assert(db->private_in_wal == before->private_in_wal);
}

static void time_recovery(void)
{
    struct timelite_batches db;
    struct timelite_record record = {7, 10, 1};
    uint64_t sequence;
    size_t count;
    int boundary, after, survive, i;
    /* Different timestamps ensure selecting an old manifest cannot silently
     * restore its older time after recovery of newer WAL commits. */
    for (boundary = 1; boundary <= 8; boundary++)
    {
        for (after = 0; after <= 1; after++)
        {
            for (survive = 0; survive <= 1; survive++)
            {
                TEST_CASE("time recovery checkpoint boundary", boundary * 100 + after * 10 + survive);
                new_db(&db);
                record.timestamp_us = 10;
                assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
                assert(checkpoint(&db) == 0);
                record.timestamp_us = 20;
                assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
                operation = 0;
                if (boundary <= 6)
                {
                    fail_operation = boundary;
                    fail_after = after;
                }
                else
                {
                    truncate_failure = boundary - 6;
                }
                assert(checkpoint(&db) == EIO);
                assert(timelite_batches_seek(&db, 0, scratch, sizeof(scratch)) == TIMELITE_RECOVERY_REQUIRED);
                if (survive)
                {
                    persist(0);
                    persist(1);
                }
                crash();
                assert(timelite_batches_init(&db) == 0);
                assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                assert(db.private_last_time == 20 && db.private_sequence == 2);
                record.timestamp_us = 19;
                assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
                for (i = 1; i <= 2; i++)
                {
                    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
                    assert(sequence == (uint64_t)i && output[0].timestamp_us == (uint64_t)i * 10);
                }
                assert(timelite_batches_close(&db) == 0);
            }
        }
    }
    /* Every short header and manifest prefix, including complete writes with
     * a lost result. Old generation survives torn inactive-slot overwrites. */
    for (boundary = 1; boundary <= 4; boundary += 3)
    {
        for (i = 0; i <= 64; i++)
        {
            TEST_CASE("time metadata torn bytes", boundary * 100 + i);
            new_db(&db);
            record.timestamp_us = 10;
            assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
            assert(checkpoint(&db) == 0);
            record.timestamp_us = 20;
            assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
            operation = 0;
            short_operation = boundary;
            transfer_limit = (size_t)i;
            fail_operation = boundary;
            fail_after = 1;
            assert(checkpoint(&db) == EIO);
            persist(0);
            persist(1);
            crash();
            assert(timelite_batches_init(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
            assert(db.private_last_time == 20 && db.private_sequence == 2);
            assert(timelite_batches_seek(&db, 20, scratch, sizeof(scratch)) == 0);
            assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
            assert(sequence == 2 && output[0].timestamp_us == 20);
            assert(timelite_batches_close(&db) == 0);
        }
    }
}

static void indexed_reads(void)
{
    struct timelite_batches db, before;
    struct timelite_record record = {7, 0, 1};
    struct timelite_range range = {1270, 1280, 7, 1};
    uint64_t sequence;
    size_t count;
    int i, calls, previous, kind, boundary;
    new_db(&db);
    for (i = 1; i <= 128; i++)
    {
        record.timestamp_us = (uint64_t)i * 10;
        assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
        assert(checkpoint(&db) == 0);
    }
    assert(timelite_batches_close(&db) == 0);
    payload_reads = 0;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    /* Two manifest reads, no installed frame bodies. */
    assert(payload_reads == 2);
    for (i = 0; i <= 129; i++)
    {
        TEST_CASE("binary segment search", i);
        payload_reads = 0;
        assert(timelite_batches_seek(&db, (uint64_t)i * 10, scratch, sizeof(scratch)) ==
               (i == 129 ? TIMELITE_END : 0));
        assert(payload_reads == (i == 129 ? 0 : 1));
        if (i != 129)
        {
            assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
            assert(sequence == (uint64_t)(i == 0 ? 1 : i));
        }
    }
    for (kind = 0; kind < 2; kind++)
    {
        assert(timelite_batches_rewind(&db) == 0);
        read_calls = 0;
        if (kind == 0)
        {
            assert(timelite_batches_seek(&db, 1270, scratch, sizeof(scratch)) == 0);
        }
        else
        {
            assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
        }
        calls = read_calls;
        for (boundary = 1; boundary <= calls; boundary++)
        {
            TEST_CASE(kind ? "range read failure" : "seek read failure", boundary);
            assert(timelite_batches_rewind(&db) == 0);
            before = db;
            read_calls = 0;
            fail_read_at = boundary;
            previous = writes;
            count = 99;
            sequence = 99;
            output[0].value = 987;
            if (kind == 0)
            {
                assert(timelite_batches_seek(&db, 1270, scratch, sizeof(scratch)) == EIO);
            }
            else
            {
                assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == EIO);
            }
            assert_cursor(&db, &before);
            assert(count == 99 && sequence == 99 && output[0].value == 987 && writes == previous);
            fail_read_at = 0;
        }
    }
    /* Stop at the upper bound without reading later damaged payloads. */
    range.from_us = 0;
    range.until_us = 1;
    assert(timelite_batches_rewind(&db) == 0);
    before = db;
    payload_reads = 0;
    assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_END);
    assert(payload_reads == 1);
    assert_cursor(&db, &before);
    db.private_failed = 1;
    assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_RECOVERY_REQUIRED);
    assert_cursor(&db, &before);
    assert(timelite_batches_close(&db) == 0);
}

static void legacy_unordered(void)
{
    struct timelite_batches db;
    struct timelite_record record = {7, 10, 1};
    struct timelite_range range = {0, 50, 0, 0};
    uint64_t sequence;
    size_t count;
    int i;
    TEST_CASE("legacy unordered WAL and spans", 0);
    new_db(&db);
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    record.timestamp_us = 20;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_close(&db) == 0);
    /* Valid 004 frames with decreasing times, accepted by the 006 writer. */
    wal_bytes[68] = 100;
    encode_fixture32(wal_bytes + 108, fixture_crc_value(wal_bytes + 32, 52));
    fixture_crc(wal_bytes + 84, 28);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_last_time == 20 && !db.private_wal_ordered);
    assert(checkpoint(&db) == 0);
    assert(memcmp(database_bytes + 160, golden_unordered, 64) == 0);
    assert(database_bytes[188] == 20 && database_bytes[196] == 100);
    record.timestamp_us = 30;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_last_time == 30);
    {
        struct timelite_aggregate aggregate;
        assert(timelite_batches_aggregate_range(&db, &range, &aggregate, scratch, sizeof(scratch)) == 0);
        assert(aggregate.record_count == 2 && aggregate.minimum_value == 1 && aggregate.maximum_value == 1);
    }
    assert(timelite_batches_seek(&db, 50, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
    assert(sequence == 1 && output[0].timestamp_us == 100);
    assert(timelite_batches_rewind(&db) == 0);
    for (i = 2; i <= 3; i++)
    {
        assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
        assert(sequence == (uint64_t)i && output[0].timestamp_us == (uint64_t)i * 10);
    }
    assert(timelite_batches_next_range(&db, &range, output, 64, &count, &sequence, scratch, sizeof(scratch)) == TIMELITE_END);
    /* Mix a real 006 header with later span-bearing segments. */
    assert(timelite_batches_close(&db) == 0);
    memmove(database_bytes + 192, database_bytes + 224, files[0].length - 224);
    files[0].length -= 32;
    memcpy(database_bytes + 160, "TLSEGMNT", 8);
    fixture_crc(database_bytes + 160, 28);
    encode_fixture32(database_bytes + 32 + 16, (uint32_t)files[0].length);
    encode_fixture32(database_bytes + 32 + 40, 360);
    fixture_crc(database_bytes + 32, 60);
    files[0].dirty = 0;
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    record.timestamp_us = 110;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_expire_before(&db, 50, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_rewind(&db) == 0);
    for (i = 1; i <= 4; i++)
    {
        if (i == 3)
        {
            continue;
        }
        assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0);
        assert(sequence == (uint64_t)i);
    }
    assert(timelite_batches_close(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_installed == 4 && db.private_live_batches == 3);
    assert(timelite_batches_expire_before(&db, 101, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_rewind(&db) == 0);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0 && sequence == 4);
    assert(timelite_batches_close(&db) == 0);
    /* Zero is a persisted timestamp, distinguished by marker 7. */
    new_db(&db);
    record.timestamp_us = 0;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(db.private_last_time == 0 && database_bytes[144] == 7);
    assert(timelite_batches_close(&db) == 0);

}

static void aggregate_errors(void)
{
    struct timelite_batches db, before;
    struct timelite_record record = {7, 10, -1};
    struct timelite_range range = {0, 100, 0, 0};
    struct timelite_aggregate result, sentinel = {99, 123, 456};
    uint64_t sequence;
    int calls, boundary, kind;
    new_db(&db);
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    record.timestamp_us = 20;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    record.timestamp_us = 30;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_seek(&db, 100, scratch, sizeof(scratch)) == TIMELITE_END);
    before = db;
    aggregate_checking = 1;
    read_calls = 0;
    assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
    assert(result.record_count == 3 && result.minimum_value == -1 && result.maximum_value == -1);
    calls = read_calls;
    assert(memcmp(&db, &before, sizeof(db)) == 0);
    for (boundary = 1; boundary <= calls; boundary++)
    {
        TEST_CASE("aggregate read failure preserves result and handle", boundary);
        result = sentinel;
        read_calls = 0;
        fail_read_at = boundary;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == EIO);
        assert(memcmp(&result, &sentinel, sizeof(result)) == 0);
        assert(memcmp(&db, &before, sizeof(db)) == 0);
        assert(files[0].owned && files[1].owned);
    }
    fail_read_at = 0;
    for (kind = 0; kind < 3; kind++)
    {
        /* First installed header, installed payload, then WAL payload. */
        unsigned char *byte = kind == 0 ? database_bytes + 160 :
                              kind == 1 ? database_bytes + 256 : wal_bytes + 64;
        TEST_CASE("aggregate corruption preserves partial result", kind);
        *byte ^= 1;
        result = sentinel;
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == TIMELITE_INVALID_DATABASE);
        assert(memcmp(&result, &sentinel, sizeof(result)) == 0);
        assert(memcmp(&db, &before, sizeof(db)) == 0);
        *byte ^= 1;
    }
    for (kind = 0; kind < 10; kind++)
    {
        int expected = EINVAL;
        TEST_CASE("aggregate argument and handle validation", kind);
        result = sentinel;
        range.from_us = kind == 4 ? 101 : 0;
        range.filter_series = kind == 5 ? 2 : 0;
        if (kind == 6)
        {
            expected = TIMELITE_BUFFER_TOO_SMALL;
        }
        if (kind == 7)
        {
            db.private_failed = 1;
            expected = TIMELITE_RECOVERY_REQUIRED;
        }
        if (kind == 8)
        {
            aggregate_checking = 0;
            assert(timelite_batches_close(&db) == 0);
            aggregate_checking = 1;
        }
        if (kind >= 8)
        {
            expected = EBADF;
        }
        before = db;
        assert(timelite_batches_aggregate_range(kind == 0 ? NULL : &db,
                 kind == 1 ? NULL : &range, kind == 2 ? NULL : &result,
                 kind == 3 ? NULL : scratch, kind == 6 ? sizeof(scratch) - 1 : sizeof(scratch)) == expected);
        assert(memcmp(&result, &sentinel, sizeof(result)) == 0);
        assert(memcmp(&db, &before, sizeof(db)) == 0);
        db.private_failed = 0;
    }
    aggregate_checking = 0;
}

static int expire(struct timelite_batches *db, uint64_t cutoff)
{
    return timelite_batches_expire_before(db, cutoff, scratch, sizeof(scratch));
}

static void retention_fixture(struct timelite_batches *db)
{
    uint64_t i;
    new_db(db);
    for (i = 1; i <= 6; i++)
    {
        input[0].timestamp_us = i <= 2 ? 0 : (i - 2) * 10;
        append_one(db, i);
        if (i != 3 && i != 6)
        {
            assert(checkpoint(db) == 0);
        }
    }
    faults_clear();
}

static void retention_contents(struct timelite_batches *db, int expired)
{
    uint64_t sequence, i;
    size_t count;
    struct timelite_batches_status status;
    struct timelite_range range = {0, 41, 0, 0};
    struct timelite_aggregate result;
    assert(timelite_batches_rewind(db) == 0);
    for (i = expired ? 3 : 1; i <= 6; i++)
    {
        assert(timelite_batches_next(db, output, 64, &count, &sequence,
                                     scratch, sizeof(scratch)) == 0);
        assert(sequence == i && count == 1 && output[0].value == -123);
        assert(output[0].timestamp_us == (i <= 2 ? 0 : (i - 2) * 10));
    }
    assert(timelite_batches_next(db, output, 64, &count, &sequence,
                                 scratch, sizeof(scratch)) == TIMELITE_END);
    assert(timelite_batches_get_status(db, &status) == 0);
    assert(status.installed_batches == (expired ? 4 : 6) - status.pending_batches);
    assert(status.committed_batches == (expired ? 4 : 6));
    assert(status.pending_batches <= 1 && status.last_timestamp_us == 40);
    assert(status.installed_segments == (expired ? 3 : 5) - status.pending_batches);
    assert(timelite_batches_aggregate_range(db, &range, &result, scratch, sizeof(scratch)) == 0);
    assert(result.record_count == (expired ? 4 : 6));
    assert(result.minimum_value == -123 && result.maximum_value == -123);
    assert(timelite_batches_seek(db, 10, scratch, sizeof(scratch)) == 0);
    assert(timelite_batches_next_range(db, &range, output, 64, &count, &sequence,
                                       scratch, sizeof(scratch)) == 0 && sequence == 3);
}

static void retention_behavior(void)
{
    struct timelite_batches db, before;
    struct timelite_batches_status status;
    uint64_t sequence, i;
    size_t count;
    int position, prior_writes;
    new_db(&db);
    before = db;
    prior_writes = writes;
    assert(expire(&db, 0) == 0 && expire(&db, UINT64_MAX) == 0);
    assert(memcmp(&db, &before, sizeof(db)) == 0 && writes == prior_writes);
    assert(timelite_batches_close(&db) == 0);
    for (position = 0; position <= 6; position++)
    {
        TEST_CASE("retention cursor and retained queries", position);
        retention_fixture(&db);
        for (i = 0; i < (uint64_t)position; i++)
        {
            assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                         scratch, sizeof(scratch)) == 0);
        }
        before = db;
        prior_writes = writes;
        assert(expire(&db, 0) == 0);
        assert(memcmp(&db, &before, sizeof(db)) == 0 && prior_writes == writes);
        assert(expire(&db, 15) == 0);
        assert(files[0].length == 160 + 2 * 64 + 3 * 84);
        assert(files[1].length == 116);
        assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                     scratch, sizeof(scratch)) == (position == 6 ? TIMELITE_END : 0));
        if (position != 6)
        {
            assert(sequence == (position < 2 ? 3 : (uint64_t)position + 1));
        }
        retention_contents(&db, 1);
        before = db;
        assert(expire(&db, 20) == 0); /* spanning segment includes equal cutoff */
        assert(memcmp(&db, &before, sizeof(db)) == 0);
        assert(timelite_batches_close(&db) == 0);
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
        retention_contents(&db, 1);
        assert(expire(&db, UINT64_MAX) == 0);
        assert(files[0].length == 161 && files[1].length == 116);
        assert(timelite_batches_get_status(&db, &status) == 0);
        assert(status.committed_batches == 1 && status.installed_batches == 0);
        assert(checkpoint(&db) == 0); /* reclaimed front is reusable */
        assert(expire(&db, UINT64_MAX) == 0);
        assert(timelite_batches_get_status(&db, &status) == 0);
        assert(status.committed_batches == 0 && status.installed_bytes == 0);
        assert(status.last_timestamp_us == 40);
        assert(timelite_batches_close(&db) == 0);
        assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
        input[0].timestamp_us = 39;
        assert(timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence) == TIMELITE_OUT_OF_ORDER);
        input[0].timestamp_us = 40;
        append_one(&db, 7);
        assert(checkpoint(&db) == 0);
        assert(timelite_batches_rewind(&db) == 0);
        assert(timelite_batches_next(&db, output, 64, &count, &sequence,
                                     scratch, sizeof(scratch)) == 0 && sequence == 7);
        assert(timelite_batches_close(&db) == 0);
    }
    retention_fixture(&db);
    before = db;
    assert(timelite_batches_expire_before(NULL, 15, scratch, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_expire_before(&db, 15, NULL, sizeof(scratch)) == EINVAL);
    assert(timelite_batches_expire_before(&db, 15, scratch, sizeof(scratch) - 1) == TIMELITE_BUFFER_TOO_SMALL);
    assert(memcmp(&db, &before, sizeof(db)) == 0);
    db.private_failed = 1;
    assert(expire(&db, 15) == TIMELITE_RECOVERY_REQUIRED);
    assert(timelite_batches_close(&db) == 0);
    assert(expire(&db, 15) == EBADF);
    input[0].timestamp_us = UINT64_C(1700000000000000);
}

static void retention_boundaries(void)
{
    struct timelite_batches db, before;
    int total, reads, boundary, after, survive, error, was_failed, complete;
    for (complete = 0; complete <= 3; complete++)
    {
        retention_fixture(&db);
        if (complete >= 2)
        {
            assert(checkpoint(&db) == 0);
            faults_clear();
        }
        assert(expire(&db, (complete & 1) ? 100 : 15) == 0);
        total = operation;
        reads = read_calls;
        assert(timelite_batches_close(&db) == 0);
        for (boundary = 1; boundary <= total + 2; boundary++)
        {
            for (after = 0; after <= 1; after++)
            {
                for (survive = 0; survive <= 1; survive++)
                {
                    TEST_CASE((complete & 1) ? "complete retention interruption" : "retention interruption",
                               boundary * 100 + after * 10 + survive);
                    retention_fixture(&db);
                    if (complete >= 2)
                    {
                        assert(checkpoint(&db) == 0);
                        faults_clear();
                    }
                    if (boundary <= total)
                    {
                        fail_operation = boundary;
                        fail_after = after;
                    }
                    else
                    {
                        truncate_failure = boundary - total;
                    }
                    assert(expire(&db, (complete & 1) ? 100 : 15) == EIO);
                    assert(files[0].owned && files[1].owned);
                    assert(expire(&db, 15) == TIMELITE_RECOVERY_REQUIRED);
                    if (survive)
                    {
                        persist(0);
                        persist(1);
                    }
                    crash();
                    assert(timelite_batches_init(&db) == 0);
                    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                    if (!(complete & 1) || db.private_generation != 0)
                    {
                        retention_contents(&db, (db.private_generation == 2 || db.private_generation == 3));
                    }
                    else
                    {
                        assert(db.private_sequence == 6 && db.private_installed == (complete >= 2 ? 6 : 5));
                        assert(db.private_last_time == 40 && db.private_end == (complete >= 2 ? 32 : 116));
                    }
                    assert(expire(&db, (complete & 1) ? 100 : 15) == 0);
                    if (!(complete & 1))
                    {
                        retention_contents(&db, 1);
                    }
                    assert(timelite_batches_close(&db) == 0);
                    assert(!files[0].owned && !files[1].owned);
                }
            }
        }
        for (boundary = 1; boundary <= reads; boundary++)
        {
            TEST_CASE("retention read failure", boundary);
            retention_fixture(&db);
            if (complete >= 2)
            {
                assert(checkpoint(&db) == 0);
                faults_clear();
            }
            before = db;
            fail_read_at = boundary;
            error = expire(&db, (complete & 1) ? 100 : 15);
            assert(error == EIO);
            was_failed = db.private_failed;
            if (!was_failed)
            {
                assert(memcmp(&db, &before, sizeof(db)) == 0);
            }
            crash();
            assert(timelite_batches_init(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
            assert(db.private_sequence == 6 && db.private_last_time == 40);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    input[0].timestamp_us = UINT64_C(1700000000000000);
}

static void retention_recovery_boundaries(void)
{
    struct timelite_batches db;
    int phase, boundary, after, survive, total, error;
    const int stop[] = {3, 11, 19};
    const int operations[] = {3, 11, 3};
    for (phase = 0; phase < 3; phase++)
    {
        total = operations[phase];
        for (boundary = 1; boundary <= total + 2; boundary++)
        {
            for (after = 0; after <= 1; after++)
            {
                for (survive = 0; survive <= 1; survive++)
                {
                    TEST_CASE("retention recovery interruption", phase * 10000 + boundary * 100 + after * 10 + survive);
                    retention_fixture(&db);
                    fail_operation = stop[phase];
                    assert(expire(&db, 15) == EIO);
                    crash();
                    assert(timelite_batches_init(&db) == 0);
                    if (boundary <= total)
                    {
                        fail_operation = boundary;
                        fail_after = after;
                    }
                    else
                    {
                        truncate_failure = boundary - total;
                    }
                    error = open_db(&db, TIMELITE_OPEN_EXISTING);
                    assert(error == EIO && !files[0].owned && !files[1].owned);
                    if (survive)
                    {
                        persist(0);
                    }
                    crash();
                    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
                    retention_contents(&db, phase != 0);
                    assert(timelite_batches_close(&db) == 0);
                }
            }
        }
    }
    input[0].timestamp_us = UINT64_C(1700000000000000);
}

static void retention_torn_writes(void)
{
    struct timelite_batches db;
    const int boundaries[] = {1, 3, 4, 9, 11, 12, 17, 20};
    size_t i, prefix, length;
    for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++)
    {
        length = boundaries[i] == 4 || boundaries[i] == 12 ? 84 : 64;
        for (prefix = 0; prefix < length; prefix++)
        {
            TEST_CASE("retention torn metadata/header/frame", boundaries[i] * 100 + (int)prefix);
            retention_fixture(&db);
            short_operation = boundaries[i];
            transfer_limit = prefix;
            assert(expire(&db, 15) == EIO);
            persist(0); /* A partial write can survive even without a sync. */
            crash();
            assert(timelite_batches_init(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
            retention_contents(&db, boundaries[i] >= 11);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    input[0].timestamp_us = UINT64_C(1700000000000000);
}

static void retention_legacy_floor(void)
{
    struct timelite_batches db;
    struct timelite_record record = {7, 100, 1};
    struct timelite_range range = {0, 50, 0, 0};
    struct timelite_aggregate result;
    uint64_t sequence;
    int phase;
    TEST_CASE("retention exposes older ordered data above legacy floor", 0);
    new_db(&db);
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(timelite_batches_close(&db) == 0);
    wal_bytes[68] = 20;
    encode_fixture32(wal_bytes + 108, fixture_crc_value(wal_bytes + 32, 52));
    fixture_crc(wal_bytes + 84, 28);
    assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
    assert(checkpoint(&db) == 0);
    record.timestamp_us = 30;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
    assert(expire(&db, 50) == 0);
    for (phase = 0; phase < 3; phase++)
    {
        assert(timelite_batches_aggregate_range(&db, &range, &result, scratch, sizeof(scratch)) == 0);
        assert(result.record_count == 1 && result.minimum_value == 1);
        if (phase == 0)
        {
            assert(timelite_batches_close(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
        }
        else if (phase == 1)
        {
            assert(checkpoint(&db) == 0);
        }
    }
    assert(timelite_batches_close(&db) == 0);
}

static void retention_end_and_checkpoint(void)
{
    struct timelite_batches db;
    uint64_t sequence;
    size_t count;
    int boundary, after;
    TEST_CASE("retention END sees future append", 0);
    new_db(&db);
    input[0].timestamp_us = 0;
    append_one(&db, 1);
    assert(checkpoint(&db) == 0);
    assert(timelite_batches_seek(&db, 1, scratch, sizeof(scratch)) == TIMELITE_END);
    assert(expire(&db, 1) == 0);
    append_one(&db, 2);
    assert(timelite_batches_next(&db, output, 64, &count, &sequence, scratch, sizeof(scratch)) == 0 && sequence == 2);
    input[0].timestamp_us = UINT64_MAX;
    append_one(&db, 3);
    assert(checkpoint(&db) == 0);
    assert(expire(&db, UINT64_MAX) == 0);
    assert(db.private_live_batches == 2); /* mixed zero/MAX segment stays whole */
    assert(timelite_batches_close(&db) == 0);
    for (boundary = 1; boundary <= 8; boundary++)
    {
        for (after = 0; after <= 1; after++)
        {
            TEST_CASE("checkpoint after retention revision", boundary * 10 + after);
            retention_fixture(&db);
            assert(expire(&db, 15) == 0);
            faults_clear();
            if (boundary <= 6)
            {
                fail_operation = boundary;
                fail_after = after;
            }
            else
            {
                truncate_failure = boundary - 6;
            }
            assert(checkpoint(&db) == EIO);
            crash();
            assert(timelite_batches_init(&db) == 0);
            assert(open_db(&db, TIMELITE_OPEN_EXISTING) == 0);
            retention_contents(&db, 1);
            assert(timelite_batches_close(&db) == 0);
        }
    }
    input[0].timestamp_us = UINT64_C(1700000000000000);
}

int main(void)
{
    input[0].series = 17;
    input[0].timestamp_us = UINT64_C(1700000000000000);
    input[0].value = -123;
    {
        size_t i;
        for (i = 1; i < 64; i++)
        {
            input[i].timestamp_us = input[0].timestamp_us;
        }
    }
    interruptions();
    failures();
    creation();
    format_cases();
    database_status();
    checkpoint_basic();
    checkpoint_boundaries();
    checkpoint_format();
    time_recovery();
    aggregate_errors();
    indexed_reads();
    legacy_unordered();
    retention_behavior();
    retention_boundaries();
    retention_recovery_boundaries();
    retention_torn_writes();
    retention_legacy_floor();
    retention_end_and_checkpoint();
    capacity();
    puts("batch persisted/volatile model, checkpoint boundaries, recovery and 64 MiB capacity: passed");
    return 0;
}
