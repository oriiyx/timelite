/* Feature 012: the offline inspection tool against real pairs.
 *
 * Exactly one native backend is compiled into this test with renamed entry
 * points, so the library's write, sync and truncate calls pass through
 * counting wrappers that can fail before operation N. That produces real
 * files interrupted inside retention without a second crash matrix. The tool
 * itself is compiled in with its main excluded and its output captured. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define timelite_file_open real_file_open
#define timelite_file_create real_file_create
#define timelite_file_read real_file_read
#define timelite_file_write real_file_write
#define timelite_file_size real_file_size
#define timelite_file_truncate real_file_truncate
#define timelite_file_sync real_file_sync
#define timelite_file_close real_file_close
#define timelite_file_identity real_file_identity
#define timelite_file_provision real_file_provision
#if defined(_WIN32)
#include "file_io_windows.c"
#else
#include "file_io.c"
#endif
#undef timelite_file_open
#undef timelite_file_create
#undef timelite_file_read
#undef timelite_file_write
#undef timelite_file_size
#undef timelite_file_truncate
#undef timelite_file_sync
#undef timelite_file_close
#undef timelite_file_identity
#undef timelite_file_provision

#define TIMELITE_INSPECT_NO_MAIN
#include "tools/inspect/inspect.c"

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

/* Operation counter over the library's write, sync and truncate calls.
 * fail_at = N fails before the Nth operation with EIO; 0 never fails. */
static int operation, fail_at;

int timelite_file_open(struct timelite_file *file, const char *path);
int timelite_file_create(struct timelite_file *file, const char *path);
int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *transferred);
int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *transferred);
int timelite_file_size(struct timelite_file *file, uint64_t *size);
int timelite_file_truncate(struct timelite_file *file, uint64_t size);
int timelite_file_sync(struct timelite_file *file);
int timelite_file_close(struct timelite_file *file);
int timelite_file_identity(unsigned char identity[16]);
int timelite_file_provision(struct timelite_file *file, const char *path);

int timelite_file_open(struct timelite_file *file, const char *path)
{
    return real_file_open(file, path);
}

int timelite_file_create(struct timelite_file *file, const char *path)
{
    return real_file_create(file, path);
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *transferred)
{
    return real_file_read(file, offset, buffer, length, transferred);
}

int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *transferred)
{
    if (++operation == fail_at)
    {
        *transferred = 0;
        return EIO;
    }
    return real_file_write(file, offset, buffer, length, transferred);
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    return real_file_size(file, size);
}

int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    if (++operation == fail_at)
    {
        return EIO;
    }
    return real_file_truncate(file, size);
}

int timelite_file_sync(struct timelite_file *file)
{
    if (++operation == fail_at)
    {
        return EIO;
    }
    return real_file_sync(file);
}

int timelite_file_close(struct timelite_file *file)
{
    return real_file_close(file);
}

int timelite_file_identity(unsigned char identity[16])
{
    return real_file_identity(identity);
}

int timelite_file_provision(struct timelite_file *file, const char *path)
{
    return real_file_provision(file, path);
}

/* --- fixtures ----------------------------------------------------------- */

#define FIXTURE_LIMIT 65536

static unsigned char scratch[TIMELITE_BATCH_SCRATCH];
static unsigned char image[FIXTURE_LIMIT], saved_db[FIXTURE_LIMIT], saved_wal[FIXTURE_LIMIT];
static unsigned char before_db[FIXTURE_LIMIT], before_wal[FIXTURE_LIMIT];
static char captured[FIXTURE_LIMIT + 1];
static char capture_path[4608];

static void encode_fixture(unsigned char *bytes, uint64_t value, size_t width)
{
    size_t i;
    for (i = 0; i < width; i++)
    {
        bytes[i] = (unsigned char)(value >> (8 * i));
    }
}

/* Independent CRC-32 so fixtures do not depend on the tool or the library. */
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

/* Plain stdio, so fixtures exist even where the backend cannot provision. */
static void write_file(const char *path, const unsigned char *bytes, size_t length)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(length == 0 || fwrite(bytes, 1, length, f) == length);
    assert(fclose(f) == 0);
}

static size_t read_file(const char *path, unsigned char *bytes)
{
    FILE *f = fopen(path, "rb");
    size_t length;
    assert(f != NULL);
    length = fread(bytes, 1, FIXTURE_LIMIT, f);
    assert(!ferror(f) && length < FIXTURE_LIMIT);
    assert(fclose(f) == 0);
    return length;
}

static void flip_byte(const char *path, size_t offset)
{
    size_t length = read_file(path, image);
    assert(offset < length);
    image[offset] ^= 1;
    write_file(path, image, length);
}

static void cut_file(const char *path, size_t length)
{
    size_t have = read_file(path, image);
    assert(length <= have);
    write_file(path, image, length);
}

/* Feature 004 header and the feature 006 creation image (generation 0
 * manifest in slot 0 with documented CRC 0x73b64024, zero slot 1). */
static void synthetic_pair(const char *db, const char *wal, unsigned char identity_byte)
{
    memset(image, 0, 160);
    memcpy(image, "TIMELITE", 8);
    encode_fixture(image + 8, 2, 4);
    memset(image + 12, identity_byte, 16);
    encode_fixture(image + 28, fixture_crc(image, 28), 4);
    memcpy(image + 32, "TLINSTAL", 8);
    encode_fixture(image + 48, 160, 8);
    encode_fixture(image + 92, fixture_crc(image + 32, 60), 4);
    assert(fixture_crc(image + 32, 60) == UINT32_C(0x73b64024));
    write_file(db, image, 160);
    memcpy(image, "TIMEWAL!", 8);
    encode_fixture(image + 28, fixture_crc(image, 28), 4);
    write_file(wal, image, 32);
}

/* --- running the tool --------------------------------------------------- */

static int run_tool(int argc, char **argv)
{
    FILE *f = fopen(capture_path, "w+b");
    size_t length;
    int code;
    assert(f != NULL);
    code = timelite_inspect_run(argc, argv, f, f);
    assert(fflush(f) == 0 && fseek(f, 0, SEEK_SET) == 0);
    captured[0] = '\n';
    length = fread(captured + 1, 1, FIXTURE_LIMIT - 1, f);
    assert(!ferror(f) && length < FIXTURE_LIMIT - 1);
    captured[length + 1] = '\0';
    assert(fclose(f) == 0);
    return code;
}

static int has_line(const char *line)
{
    char needle[256];
    int length = snprintf(needle, sizeof(needle), "\n%s\n", line);
    assert(length > 0 && (size_t)length < sizeof(needle));
    return strstr(captured, needle) != NULL;
}

/* Value of the first line starting with key=, copied into out. */
static int value_of(const char *text, const char *key, char *out, size_t capacity)
{
    char needle[128];
    const char *start, *end;
    int length = snprintf(needle, sizeof(needle), "\n%s=", key);
    assert(length > 0 && (size_t)length < sizeof(needle));
    start = strstr(text, needle);
    if (start == NULL)
    {
        return 0;
    }
    start += length;
    end = strchr(start, '\n');
    assert(end != NULL && (size_t)(end - start) < capacity);
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return 1;
}

#define EXPECT(line) do { if (!has_line(line)) { \
    fprintf(stderr, "missing line: %s\noutput:%s", line, captured); assert(has_line(line)); } } while (0)

/* verify must leave both files byte-identical. */
static int verify(const char *db, const char *wal)
{
    char *argv[] = {"timelite-inspect", "verify", NULL, NULL};
    size_t db_length = read_file(db, before_db), wal_length = read_file(wal, before_wal);
    int code;
    argv[2] = (char *)db;
    argv[3] = (char *)wal;
    code = run_tool(4, argv);
    EXPECT("mode=verify");
    EXPECT("read_only=1");
    assert(read_file(db, image) == db_length && memcmp(image, before_db, db_length) == 0);
    assert(read_file(wal, image) == wal_length && memcmp(image, before_wal, wal_length) == 0);
    return code;
}

static int status(const char *db, const char *wal, int dump)
{
    char *argv[] = {"timelite-inspect", "status", NULL, NULL, "--dump"};
    int code;
    argv[2] = (char *)db;
    argv[3] = (char *)wal;
    code = run_tool(dump ? 5 : 4, argv);
    EXPECT("mode=status");
    EXPECT("read_only=0");
    return code;
}

/* Runs verify, then status on the same bytes, and checks that status reports
 * exactly the open result and every status field that verify predicted. */
static const char *status_keys[] = {"committed_batches", "installed_batches", "pending_batches",
                                    "installed_segments", "wal_bytes", "installed_bytes",
                                    "last_timestamp_us"};

/* After agree returns, captured holds the verify output again. */
static char predicted[FIXTURE_LIMIT + 1];

static int agree(const char *db, const char *wal, int expected_verify_code)
{
    char result[128], expected[128], actual[128], key[64];
    size_t i;
    int code, status_code;
    code = verify(db, wal);
    if (code != expected_verify_code)
    {
        fprintf(stderr, "verify exit %d, expected %d\noutput:%s", code, expected_verify_code, captured);
        assert(code == expected_verify_code);
    }
    memcpy(predicted, captured, sizeof(predicted));
    assert(value_of(predicted, "recovery.result", result, sizeof(result)));
    status_code = status(db, wal, 0);
    if (strcmp(result, "ok") == 0)
    {
        assert(status_code == 0);
        EXPECT("open_result=ok");
        for (i = 0; i < sizeof(status_keys) / sizeof(status_keys[0]); i++)
        {
            assert(snprintf(key, sizeof(key), "predicted.%s", status_keys[i]) < (int)sizeof(key));
            assert(value_of(predicted, key, expected, sizeof(expected)));
            assert(value_of(captured, status_keys[i], actual, sizeof(actual)));
            if (strcmp(expected, actual) != 0)
            {
                fprintf(stderr, "%s: verify predicted %s, status reports %s\n", status_keys[i], expected, actual);
                assert(strcmp(expected, actual) == 0);
            }
        }
    }
    else
    {
        assert(snprintf(key, sizeof(key), "open_error=%s", result) < (int)sizeof(key));
        EXPECT(key);
        assert(status_code == 1);
    }
    memcpy(captured, predicted, sizeof(captured));
    return status_code;
}

/* --- library fixtures --------------------------------------------------- */

static struct timelite_batches db;

static int open_pair(const char *path, const char *wal_path, enum timelite_open_mode mode)
{
    assert(timelite_batches_init(&db) == 0);
    return timelite_batches_open(&db, path, wal_path, mode, scratch, sizeof(scratch));
}

static void append(uint32_t series, uint64_t time, int64_t value)
{
    struct timelite_record record;
    uint64_t sequence;
    record.series = series;
    record.timestamp_us = time;
    record.value = value;
    assert(timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence) == 0);
}

static void checkpoint(void)
{
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == 0);
}

/* Segments 1 (sequence 1, time 10) and 2 (sequence 2, time 20), pending
 * sequence 3 at time 30. */
static void retention_fixture(const char *path, const char *wal_path)
{
    assert(open_pair(path, wal_path, TIMELITE_CREATE_NEW) == 0);
    append(7, 10, 100);
    checkpoint();
    append(7, 20, 200);
    checkpoint();
    append(8, 30, 300);
}

static void remove_pair(const char *path, const char *wal_path)
{
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
    char path[4608], wal_path[4608], path2[4608], wal2_path[4608], v1_path[4608];
    char phase[64], line[128];
    struct timelite_db legacy;
    char *usage[] = {"timelite-inspect", "verify", NULL};
    char *unknown[] = {"timelite-inspect", "repair", "a", "b"};
    char *filtered[] = {"timelite-inspect", "status", NULL, NULL, "--dump", "--from-us", "20",
                        "--until-us", "31", "--series", "7"};
    char *bad_filter[] = {"timelite-inspect", "status", NULL, NULL, "--from-us", "20"};
    size_t wal_length, db_length;
    int error, k, total, seen_prepare = 0, seen_tail = 0, seen_cleanup = 0;

    assert(make_test_directory(directory, sizeof(directory)));
    assert(snprintf(path, sizeof(path), "%s/inspect.db", directory) < (int)sizeof(path));
    assert(snprintf(wal_path, sizeof(wal_path), "%s/inspect.wal", directory) < (int)sizeof(wal_path));
    assert(snprintf(path2, sizeof(path2), "%s/second.db", directory) < (int)sizeof(path2));
    assert(snprintf(wal2_path, sizeof(wal2_path), "%s/second.wal", directory) < (int)sizeof(wal2_path));
    assert(snprintf(v1_path, sizeof(v1_path), "%s/legacy.tl", directory) < (int)sizeof(v1_path));
    assert(snprintf(capture_path, sizeof(capture_path), "%s/output.txt", directory) < (int)sizeof(capture_path));

    /* Usage and I/O errors exit 2 before touching anything. */
    TEST_CASE("usage", 0);
    assert(run_tool(2, usage) == 2);
    assert(run_tool(4, unknown) == 2);
    {
        char *missing[] = {"timelite-inspect", "verify", NULL, NULL};
        missing[2] = path;
        missing[3] = wal_path;
        assert(run_tool(4, missing) == 2);
        assert(strstr(captured, "\nerror=cannot open database") != NULL);
    }

    /* Synthetic fixtures run everywhere, including where provisioning is
     * unsupported: the documented creation image, a v1 file, a second
     * identity and a damaged slot. */
    TEST_CASE("synthetic fresh pair", 0);
    synthetic_pair(path, wal_path, 0x42);
    assert(verify(path, wal_path) == 0);
    EXPECT("database_version=2");
    EXPECT("database_identity=42424242424242424242424242424242");
    EXPECT("pair_identity=match");
    EXPECT("slot.0.state=valid");
    EXPECT("slot.0.revision=0");
    EXPECT("slot.1.state=empty");
    EXPECT("active_slot=0");
    EXPECT("manifest_kind=install-006");
    EXPECT("generation=0");
    EXPECT("database_logical_end=160");
    EXPECT("wal_frames=0");
    EXPECT("wal_logical_end=32");
    EXPECT("committed_batches=0");
    EXPECT("database_trailing_bytes=0");
    EXPECT("recovery.result=ok");
    EXPECT("recovery.wal_action=none");
    EXPECT("predicted.installed_bytes=0");
    EXPECT("inconsistencies=0");

    TEST_CASE("synthetic v1 file", 0);
    assert(timelite_init(&legacy) == 0);
    assert(timelite_open(&legacy, v1_path, TIMELITE_CREATE_NEW) == 0);
    assert(timelite_close(&legacy) == 0);
    assert(verify(v1_path, wal_path) == 1);
    EXPECT("database_version=1");
    EXPECT("recovery.result=TIMELITE_UNSUPPORTED_VERSION");
    EXPECT("inconsistency.1.offset=8");
    assert(verify(path, v1_path) == 1);
    EXPECT("recovery.result=TIMELITE_INVALID_DATABASE");
    EXPECT("inconsistency.1.file=wal");

    TEST_CASE("synthetic mismatched pair", 0);
    synthetic_pair(path2, wal2_path, 0x43);
    assert(verify(path, wal2_path) == 1);
    EXPECT("pair_identity=mismatch");
    EXPECT("recovery.result=TIMELITE_PAIR_MISMATCH");
    EXPECT("inconsistencies=1");

    TEST_CASE("synthetic damaged slot", 0);
    flip_byte(path, 40);
    assert(verify(path, wal_path) == 1);
    EXPECT("slot.0.state=invalid");
    EXPECT("slot.0.reason=checksum mismatch");
    EXPECT("active_slot=none");
    EXPECT("recovery.result=ok");
    EXPECT("predicted.committed_batches=0");
    EXPECT("inconsistency.1.offset=32");
    synthetic_pair(path, wal_path, 0x42);

    TEST_CASE("synthetic damaged header", 0);
    flip_byte(wal_path, 3);
    assert(verify(path, wal_path) == 1);
    EXPECT("inconsistency.1.detail=wrong magic");
    EXPECT("recovery.result=TIMELITE_INVALID_DATABASE");
    synthetic_pair(path, wal_path, 0x42);

    /* Real pairs need durable provisioning. Where it is unsupported the
     * status mode must report the library's ENOTSUP and exit 2. */
    TEST_CASE("status without provisioning", 0);
    remove_pair(path2, wal2_path);
    error = open_pair(path2, wal2_path, TIMELITE_CREATE_NEW);
    if (error == ENOTSUP)
    {
        assert(status(path, wal_path, 0) == 2);
        EXPECT("open_error=ENOTSUP");
        assert(read_file(path, image) == 160);
        remove_pair(path, wal_path);
        (void)remove_test_file(path2);
        (void)remove_test_file(wal2_path);
        assert(remove_test_file(v1_path) == 0);
        assert(remove_test_file(capture_path) == 0);
        assert(remove_test_directory(directory) == 0);
        puts("inspect: verify exercised on synthetic pairs; status reported ENOTSUP as the library does");
        return 77;
    }
#if defined(_WIN32)
    assert(error == ENOTSUP);
#endif
    assert(error == 0);
    assert(timelite_batches_close(&db) == 0);
    /* The library's fresh pair equals the documented image; the identity
     * differs, so compare the manifest slots and verify the whole pair. */
    assert(read_file(path2, image) == 160);
    assert(memcmp(image + 32, "TLINSTAL", 8) == 0 && fixture_crc(image + 32, 60) == UINT32_C(0x73b64024));
    TEST_CASE("library fresh pair", 0);
    assert(agree(path2, wal2_path, 0) == 0);
    EXPECT("inconsistencies=0");
    EXPECT("manifest_kind=install-006");

    TEST_CASE("pending wal", 0);
    assert(open_pair(path2, wal2_path, TIMELITE_OPEN_EXISTING) == 0);
    append(7, 10, 100);
    assert(timelite_batches_close(&db) == 0);
    assert(agree(path2, wal2_path, 0) == 0);
    EXPECT("wal_frames=1");
    EXPECT("pending_batches=1");
    EXPECT("wal_first_sequence=1");
    EXPECT("timestamp_floor_source=wal");
    EXPECT("predicted.wal_bytes=116");

    TEST_CASE("after checkpoint", 0);
    assert(open_pair(path2, wal2_path, TIMELITE_OPEN_EXISTING) == 0);
    checkpoint();
    append(7, 20, 200);
    assert(timelite_batches_close(&db) == 0);
    assert(agree(path2, wal2_path, 0) == 0);
    EXPECT("active_slot=1");
    EXPECT("manifest_kind=install-007");
    EXPECT("generation=1");
    EXPECT("segment.1.kind=TLSPAN07");
    EXPECT("segment.1.first_sequence=1");
    EXPECT("segment.1.frames_read=1");
    EXPECT("installed_batches=1");
    EXPECT("pending_batches=1");
    EXPECT("timestamp_min_us=10");
    EXPECT("timestamp_max_us=20");
    EXPECT("predicted.installed_bytes=148");
    assert(status(path2, wal2_path, 1) == 0);
    EXPECT("batch=1 series=7 us=10 value=100");
    EXPECT("batch=2 series=7 us=20 value=200");
    filtered[2] = path2;
    filtered[3] = wal2_path;
    assert(run_tool(11, filtered) == 0);
    assert(!has_line("batch=1 series=7 us=10 value=100"));
    EXPECT("batch=2 series=7 us=20 value=200");
    bad_filter[2] = path2;
    bad_filter[3] = wal2_path;
    assert(run_tool(6, bad_filter) == 2);
    db_length = read_file(path2, saved_db);
    wal_length = read_file(wal2_path, saved_wal);
    assert(db_length == 308 && wal_length == 116);

    /* Incomplete WAL suffix: recovery truncates it and loses no commit. */
    TEST_CASE("incomplete wal suffix", 0);
    cut_file(wal2_path, 100);
    assert(agree(path2, wal2_path, 0) == 0);
    EXPECT("wal_incomplete_suffix_offset=32");
    EXPECT("wal_incomplete_suffix_bytes=68");
    EXPECT("recovery.wal_truncate_to=32");
    EXPECT("pending_batches=0");
    assert(read_file(wal2_path, image) == 32);
    EXPECT("inconsistencies=0");

    /* 1..31 bytes of header: ambiguous, the library fails closed. */
    TEST_CASE("ambiguous short suffix", 0);
    write_file(wal2_path, saved_wal, wal_length);
    cut_file(wal2_path, 40);
    assert(agree(path2, wal2_path, 1) == 1);
    EXPECT("recovery.result=TIMELITE_INVALID_DATABASE");
    EXPECT("inconsistency.1.offset=32");
    assert(read_file(wal2_path, image) == 40);

    /* Damaged committed WAL frame: fail closed, never truncate. */
    TEST_CASE("damaged wal frame", 0);
    write_file(wal2_path, saved_wal, wal_length);
    flip_byte(wal2_path, 70);
    assert(agree(path2, wal2_path, 1) == 1);
    EXPECT("inconsistency.1.file=wal");
    EXPECT("inconsistency.1.offset=32");
    EXPECT("inconsistency.1.detail=body checksum mismatch");
    EXPECT("recovery.result=TIMELITE_INVALID_DATABASE");
    assert(read_file(wal2_path, image) == wal_length);

    /* Damaged installed frame: open succeeds, the read fails at sequence 1. */
    TEST_CASE("damaged installed frame", 0);
    write_file(wal2_path, saved_wal, wal_length);
    flip_byte(path2, 260);
    assert(agree(path2, wal2_path, 1) == 0);
    EXPECT("recovery.result=ok");
    EXPECT("first_unreadable_sequence=1");
    EXPECT("recovery.read_fails_at_sequence=1");
    EXPECT("inconsistency.1.file=database");
    EXPECT("inconsistency.1.offset=224");
    assert(status(path2, wal2_path, 1) == 1);
    EXPECT("read_error=TIMELITE_INVALID_DATABASE");

    /* Newest slot damaged: the older manifest is chosen and the pending WAL
     * frame no longer continues from it, so the library fails closed. */
    TEST_CASE("damaged newest slot", 0);
    write_file(path2, saved_db, db_length);
    flip_byte(path2, 100);
    assert(agree(path2, wal2_path, 1) == 1);
    EXPECT("slot.1.state=invalid");
    EXPECT("active_slot=0");
    EXPECT("recovery.result=TIMELITE_INVALID_DATABASE");
    /* Same damage with an empty WAL: orphan bytes without pending frames. */
    cut_file(wal2_path, 32);
    assert(agree(path2, wal2_path, 1) == 1);
    EXPECT("recovery.fails_at=orphan bytes");
    EXPECT("inconsistency.2.offset=160");

    /* Older slot damaged: the newest manifest still opens. */
    TEST_CASE("damaged older slot", 0);
    write_file(path2, saved_db, db_length);
    write_file(wal2_path, saved_wal, wal_length);
    flip_byte(path2, 40);
    assert(agree(path2, wal2_path, 1) == 0);
    EXPECT("slot.0.state=invalid");
    EXPECT("active_slot=1");
    EXPECT("recovery.result=ok");
    EXPECT("inconsistencies=1");
    write_file(path2, saved_db, db_length);

    /* Mismatched real pair. */
    TEST_CASE("mismatched real pair", 0);
    remove_pair(path, wal_path);
    assert(open_pair(path, wal_path, TIMELITE_CREATE_NEW) == 0);
    assert(timelite_batches_close(&db) == 0);
    assert(agree(path2, wal_path, 1) == 1);
    EXPECT("pair_identity=mismatch");
    EXPECT("recovery.result=TIMELITE_PAIR_MISMATCH");
    remove_pair(path2, wal2_path);

    /* Checkpoint interrupted before the WAL truncate: the manifest is durable
     * and the WAL is stale, so recovery finishes the reclaim. */
    TEST_CASE("stale wal after interrupted reclaim", 0);
    assert(open_pair(path, wal_path, TIMELITE_OPEN_EXISTING) == 0);
    append(7, 10, 100);
    operation = 0;
    fail_at = 6;
    assert(timelite_batches_checkpoint(&db, scratch, sizeof(scratch)) == EIO);
    fail_at = 0;
    assert(timelite_batches_close(&db) == 0);
    assert(read_file(wal_path, image) == 116);
    assert(agree(path, wal_path, 0) == 0);
    EXPECT("wal_stale=1");
    EXPECT("wal_frames=1");
    EXPECT("pending_batches=0");
    EXPECT("installed_batches=1");
    EXPECT("recovery.wal_truncate_to=32");
    EXPECT("recovery.wal_action=finish interrupted reclaim: every WAL frame is already installed");
    EXPECT("predicted.wal_bytes=32");
    assert(read_file(wal_path, image) == 32);
    remove_pair(path, wal_path);

    /* Retention interrupted before every write, sync and truncate. Each
     * on-disk state must verify as coherent, status must recover exactly as
     * predicted, and the recovered pair must verify clean again. */
    TEST_CASE("retention operation count", 0);
    retention_fixture(path, wal_path);
    operation = 0;
    assert(timelite_batches_expire_before(&db, 15, scratch, sizeof(scratch)) == 0);
    total = operation;
    assert(total >= 12);
    assert(timelite_batches_close(&db) == 0);
    assert(agree(path, wal_path, 0) == 0);
    EXPECT("manifest_kind=retention-010");
    EXPECT("retention_phase=NORMAL");
    EXPECT("generation=1");
    EXPECT("segment.1.first_sequence=2");
    EXPECT("installed_last_sequence=2");
    EXPECT("pending_batches=1");
    EXPECT("predicted.last_timestamp_us=30");
    remove_pair(path, wal_path);
    for (k = 1; k <= total; k++)
    {
        TEST_CASE("retention interrupted before operation", k);
        retention_fixture(path, wal_path);
        operation = 0;
        fail_at = k;
        assert(timelite_batches_expire_before(&db, 15, scratch, sizeof(scratch)) == EIO);
        fail_at = 0;
        assert(timelite_batches_close(&db) == 0);
        assert(agree(path, wal_path, 0) == 0);
        assert(value_of(captured, "retention_phase", phase, sizeof(phase)) ||
               value_of(captured, "manifest_kind", phase, sizeof(phase)));
        seen_prepare |= strcmp(phase, "PREPARE") == 0;
        seen_tail |= strcmp(phase, "TAIL") == 0;
        seen_cleanup |= strcmp(phase, "CLEANUP") == 0;
        if (strcmp(phase, "PREPARE") == 0)
        {
            EXPECT("generation=2");
            EXPECT("recovery.database_truncate_to=456");
            assert(snprintf(line, sizeof(line), "recovery.retention_action=PREPARE: discard the unfinished tail beyond data_end, truncate, publish NORMAL") < (int)sizeof(line));
            EXPECT(line);
        }
        if (strcmp(phase, "TAIL") == 0)
        {
            EXPECT("root=456");
            EXPECT("database_superseded_front_bytes=296");
            EXPECT("predicted.installed_bytes=148");
        }
        if (strcmp(phase, "CLEANUP") == 0)
        {
            EXPECT("recovery.database_truncate_to=308");
        }
        /* The recovered pair is coherent, in NORMAL phase, without tails. */
        assert(agree(path, wal_path, 0) == 0);
        if (strcmp(phase, "install-007") != 0)
        {
            EXPECT("retention_phase=NORMAL");
        }
        EXPECT("database_trailing_bytes=0");
        EXPECT("wal_trailing_bytes=0");
        EXPECT("inconsistencies=0");
        assert(status(path, wal_path, 1) == 0);
        EXPECT("batch=3 series=8 us=30 value=300");
        remove_pair(path, wal_path);
    }
    assert(seen_prepare && seen_tail && seen_cleanup);

    assert(remove_test_file(v1_path) == 0);
    assert(remove_test_file(capture_path) == 0);
    assert(remove_test_directory(directory) == 0);
    puts("inspect: verify and status agree on fresh, pending, checkpointed, damaged, mismatched and interrupted-retention pairs");
    return 0;
}
