#include "timelite.h"
#include "file_io.h"

#include <errno.h>
#include <limits.h>
#include <float.h>
#include <string.h>

typedef char timelite_byte_must_be_eight_bits[CHAR_BIT == 8 ? 1 : -1];

static struct timelite_file get_file(const struct timelite_db *db)
{
    struct timelite_file file;
#if defined(_WIN32)
    file.handle = db->private_handle;
#else
    file.fd = db->private_fd;
#endif
    return file;
}

static void set_file(struct timelite_db *db, struct timelite_file file)
{
#if defined(_WIN32)
    db->private_handle = file.handle;
#else
    db->private_fd = file.fd;
#endif
}

static int is_closed(struct timelite_file file)
{
#if defined(_WIN32)
    return file.handle == NULL;
#else
    return file.fd == -1;
#endif
}

int timelite_init(struct timelite_db *db)
{
    struct timelite_file file = TIMELITE_FILE_INIT;
    if (db == NULL)
    {
        return EINVAL;
    }
    set_file(db, file);
    return 0;
}

static int initialize_file(struct timelite_file *file)
{
    unsigned char header[12];
    uint32_t version = 1;
    size_t count;
    unsigned int i;
    int error;

    memcpy(header, "TIMELITE", 8);
    for (i = 0; i < 4; i++)
    {
        header[8 + i] = (unsigned char)(version >> (8 * i));
    }
    error = timelite_file_write(file, 0, header, sizeof(header), &count);
    if (error != 0)
    {
        return error;
    }
    if (count != sizeof(header))
    {
        return EIO;
    }
    return timelite_file_sync(file);
}

static int validate_file(struct timelite_file *file)
{
    unsigned char header[12];
    uint32_t version = 0;
    uint64_t size;
    size_t count;
    unsigned int i;
    int error;

    error = timelite_file_read(file, 0, header, sizeof(header), &count);
    if (error != 0)
    {
        return error;
    }
    if (count != sizeof(header) || memcmp(header, "TIMELITE", 8) != 0)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    for (i = 0; i < 4; i++)
    {
        version |= (uint32_t)header[8 + i] << (8 * i);
    }
    if (version == 0)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    if (version != 1)
    {
        return TIMELITE_UNSUPPORTED_VERSION;
    }
    error = timelite_file_size(file, &size);
    if (error != 0)
    {
        return error;
    }
    return size == sizeof(header) ? 0 : TIMELITE_INVALID_DATABASE;
}

int timelite_open(struct timelite_db *db, const char *path,
                  enum timelite_open_mode mode)
{
    struct timelite_file file;
    int error;
    int created = 0;

    if (db == NULL || path == NULL || path[0] == '\0' ||
        (mode != TIMELITE_OPEN_EXISTING && mode != TIMELITE_CREATE_NEW &&
         mode != TIMELITE_OPEN_OR_CREATE))
    {
        return EINVAL;
    }
    file = get_file(db);
    if (!is_closed(file))
    {
        return EINVAL;
    }
    if (mode == TIMELITE_CREATE_NEW)
    {
        error = timelite_file_create(&file, path);
        created = error == 0;
    }
    else
    {
        error = timelite_file_open(&file, path);
        if (error == ENOENT && mode == TIMELITE_OPEN_OR_CREATE)
        {
            error = timelite_file_create(&file, path);
            created = error == 0;
            if (error == EEXIST)
            {
                /* Another creator owns the path. Never initialize its file. */
                error = timelite_file_open(&file, path);
            }
        }
    }
    if (error != 0)
    {
        return error;
    }
    error = created ? initialize_file(&file) : validate_file(&file);
    if (error != 0)
    {
        (void)timelite_file_close(&file);
        return error;
    }
    set_file(db, file);
    return 0;
}

int timelite_close(struct timelite_db *db)
{
    struct timelite_file file;
    int error;
    if (db == NULL)
    {
        return EINVAL;
    }
    file = get_file(db);
    error = timelite_file_close(&file);
    set_file(db, file);
    return error;
}

const char *timelite_version(void)
{
    return "0.1.0-dev";
}

/* v2 uses 32-byte file headers, batch headers and commit records. Feature 006
 * adds two 64-byte install manifest slots at 32 and 96 and installed segments
 * from offset 160 in the database file. */
#define TIMELITE_DATA_START UINT64_C(160)
#define TIMELITE_SEGMENT_MIN UINT64_C(116)

static void encode32(unsigned char *p, uint32_t value)
{
    unsigned int i;
    for (i = 0; i < 4; i++)
    {
        p[i] = (unsigned char)(value >> (8 * i));
    }
}

static void encode64(unsigned char *p, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; i++)
    {
        p[i] = (unsigned char)(value >> (8 * i));
    }
}

static uint32_t decode32(const unsigned char *p)
{
    uint32_t value = 0;
    unsigned int i;
    for (i = 0; i < 4; i++)
    {
        value |= (uint32_t)p[i] << (8 * i);
    }
    return value;
}

static uint64_t decode64(const unsigned char *p)
{
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; i++)
    {
        value |= (uint64_t)p[i] << (8 * i);
    }
    return value;
}

/* CRC-32/ISO-HDLC: accidental corruption detection, not authentication. */
static uint32_t checksum(const unsigned char *p, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;
    for (i = 0; i < length; i++)
    {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}

static struct timelite_file batch_file(const struct timelite_batches *db, int wal)
{
    struct timelite_file file;
#if defined(_WIN32)
    file.handle = wal ? db->private_wal : db->private_database;
#else
    file.fd = wal ? db->private_wal : db->private_database;
#endif
    return file;
}

static void batch_set_file(struct timelite_batches *db, struct timelite_file file,
                            int wal)
{
#if defined(_WIN32)
    if (wal)
    {
        db->private_wal = file.handle;
    }
    else
    {
        db->private_database = file.handle;
    }
#else
    if (wal)
    {
        db->private_wal = file.fd;
    }
    else
    {
        db->private_database = file.fd;
    }
#endif
}

static void reset_cursor(struct timelite_batches *db)
{
    db->private_cursor = TIMELITE_DATA_START;
    db->private_segment_end = TIMELITE_DATA_START;
    db->private_in_wal = 0;
    db->private_read_sequence = 1;
}

int timelite_batches_init(struct timelite_batches *db)
{
    struct timelite_file file = TIMELITE_FILE_INIT;
    if (db == NULL)
    {
        return EINVAL;
    }
    batch_set_file(db, file, 0);
    batch_set_file(db, file, 1);
    db->private_format = 0;
    db->private_end = 32;
    db->private_sequence = 0;
    db->private_data_end = TIMELITE_DATA_START;
    db->private_installed = 0;
    db->private_generation = 0;
    db->private_revision = 0;
    db->private_live_batches = 0;
    db->private_retention = 0;
    db->private_last_time = 0;
    db->private_installed_time = 0;
    db->private_wal_first_time = 0;
    db->private_wal_minimum = 0;
    db->private_wal_maximum = 0;
    db->private_last_segment = 0;
    db->private_span_start = 1;
    db->private_wal_ordered = 1;
    db->private_legacy = 0;
    db->private_failed = 0;
    reset_cursor(db);
    return 0;
}

static int read_exact(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length)
{
    size_t count;
    int error = timelite_file_read(file, offset, buffer, length, &count);
    return error != 0 ? error : count == length ? 0 : TIMELITE_INVALID_DATABASE;
}

static int write_exact(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length)
{
    size_t count;
    int error = timelite_file_write(file, offset, buffer, length, &count);
    return error != 0 ? error : count == length ? 0 : EIO;
}

static int pair_header(struct timelite_file *file, unsigned char *header,
                        const char *magic)
{
    int error = read_exact(file, 0, header, 12);
    if (error != 0)
    {
        return error;
    }
    if (memcmp(header, magic, 8) != 0 || decode32(header + 8) == 0)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    if (decode32(header + 8) != 2 && decode32(header + 8) != 3)
    {
        return TIMELITE_UNSUPPORTED_VERSION;
    }
    error = read_exact(file, 12, header + 12, 20);
    if (error != 0)
    {
        return error;
    }
    return checksum(header, 28) == decode32(header + 28) ?
           0 : TIMELITE_INVALID_DATABASE;
}

/* Install manifest: generation g lives in slot g mod 2, so the current slot is
 * never overwritten and a torn write cannot destroy the last good install. */
static void encode_manifest(unsigned char *m, uint64_t generation,
                            uint64_t data_end, uint64_t last,
                            uint64_t last_time, uint64_t last_segment)
{
    memset(m, 0, 64);
    memcpy(m, "TLINSTAL", 8);
    encode64(m + 8, generation);
    encode64(m + 16, data_end);
    encode64(m + 24, last);
    if (generation != 0)
    {
        encode64(m + 32, last_time);
        encode64(m + 40, last_segment);
        encode32(m + 48, 7);
    }
    encode32(m + 60, checksum(m, 60));
}

static int check_manifest(const unsigned char *m, uint64_t size,
                          uint64_t *generation, uint64_t *data_end, uint64_t *last)
{
    static const unsigned char zero[28];
    uint64_t g = decode64(m + 8), d = decode64(m + 16), l = decode64(m + 24);
    if (memcmp(m, "TLINSTAL", 8) != 0 || (memcmp(m + 32, zero, 28) != 0 &&
        (decode32(m + 48) != 7 || memcmp(m + 52, zero, 8) != 0 || g == 0)) ||
        checksum(m, 60) != decode32(m + 60))
    {
        return 0;
    }
    if (d < TIMELITE_DATA_START || d > TIMELITE_DATABASE_CAPACITY || d > size)
    {
        return 0;
    }
    if (g == 0)
    {
        if (d != TIMELITE_DATA_START || l != 0)
        {
            return 0;
        }
    }
    else if (g > (TIMELITE_DATABASE_CAPACITY - TIMELITE_DATA_START) / TIMELITE_SEGMENT_MIN ||
             l < g || d < TIMELITE_DATA_START + g * TIMELITE_SEGMENT_MIN)
    {
        return 0;
    }
    *generation = g;
    *data_end = d;
    *last = l;
    return 1;
}

/* Retention revisions are independent of the surviving segment count.
 * Phase 1 discards an unfinished tail, 2 copies the tail back, 3 truncates. */
struct retention_state
{
    uint64_t revision, root;
    unsigned int phase;
    int enabled;
};

static void encode_retention(unsigned char *m, uint64_t revision,
                              uint64_t count, uint64_t end, uint64_t last,
                              uint64_t time, uint64_t tail, uint64_t root,
                              unsigned int phase)
{
    memset(m, 0, 64);
    memcpy(m, "TLRETAIN", 8);
    encode64(m + 8, revision);
    encode64(m + 16, end);
    encode64(m + 24, last);
    encode64(m + 32, time);
    encode32(m + 40, (uint32_t)tail);
    encode32(m + 44, (uint32_t)root);
    encode32(m + 48, 10);
    encode32(m + 52, (uint32_t)count);
    encode32(m + 56, phase);
    encode32(m + 60, checksum(m, 60));
}

static int retention_manifest(const unsigned char *m, uint64_t size)
{
    uint64_t end = decode64(m + 16), root = decode32(m + 44);
    uint64_t count = decode32(m + 52), tail = decode32(m + 40);
    unsigned int phase = decode32(m + 56);
    return memcmp(m, "TLRETAIN", 8) == 0 &&
           checksum(m, 60) == decode32(m + 60) && decode32(m + 48) == 10 &&
           decode64(m + 8) != 0 && phase <= 3 && root >= 160 &&
           root <= end && end <= size && end <= TIMELITE_DATABASE_CAPACITY &&
           count <= (end - root) / TIMELITE_SEGMENT_MIN &&
           count <= decode64(m + 24) &&
           (count != 0 ? tail >= root && tail < end : tail == 0 && root == end) &&
           (phase == 2 ? end - root <= root - 160 : root == 160);
}

static int publish_retention(struct timelite_file *file,
                              struct retention_state *state, uint64_t count,
                              uint64_t end, uint64_t last, uint64_t time,
                              uint64_t tail, uint64_t root, unsigned int phase)
{
    unsigned char m[64];
    int error;
    if (state->revision == UINT64_MAX)
    {
        return EOVERFLOW;
    }
    encode_retention(m, state->revision + 1, count, end, last, time, tail, root, phase);
    error = write_exact(file, 32 + 64 * ((state->revision + 1) & 1), m, 64);
    if (error == 0)
    {
        error = timelite_file_sync(file);
    }
    if (error == 0)
    {
        state->revision++;
        state->root = root;
        state->phase = phase;
        state->enabled = 1;
    }
    return error;
}

/* The body is a verbatim copy of validated WAL frames. */
struct segment_info
{
    uint64_t first, last, minimum, maximum, previous, skip;
    uint32_t body;
    size_t header_size;
    int ordered;
};

static int read_segment(struct timelite_file *file, uint64_t offset,
                         uint64_t end, struct segment_info *segment)
{
    unsigned char header[64];
    uint64_t frames;
    int error;
    if (offset > end || end - offset < 32)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    error = read_exact(file, offset, header, 32);
    if (error != 0)
    {
        return error;
    }
    segment->ordered = memcmp(header, "TLSPAN07", 8) == 0;
    segment->header_size = segment->ordered || memcmp(header, "TLUNOR07", 8) == 0 ? 64 : 32;
    if (segment->header_size == 64)
    {
        if (end - offset < 64)
        {
            return TIMELITE_INVALID_DATABASE;
        }
        error = read_exact(file, offset + 32, header + 32, 32);
        if (error != 0)
        {
            return error;
        }
    }
    else if (memcmp(header, "TLSEGMNT", 8) != 0)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    if (checksum(header, segment->header_size - 4) !=
        decode32(header + segment->header_size - 4))
    {
        return TIMELITE_INVALID_DATABASE;
    }
    segment->first = decode64(header + 8);
    segment->last = decode64(header + 16);
    segment->body = decode32(header + 24);
    if (segment->first == 0 || segment->last < segment->first ||
        segment->body > TIMELITE_WAL_CAPACITY - 32 ||
        segment->body > end - offset - segment->header_size)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    frames = segment->last - segment->first + 1;
    if (frames > segment->body / 84 ||
        segment->body > frames * TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    segment->minimum = 0;
    segment->maximum = UINT64_MAX;
    segment->previous = 0;
    segment->skip = 0;
    if (segment->header_size == 64)
    {
        segment->minimum = decode64(header + 28);
        segment->maximum = decode64(header + 36);
        segment->previous = decode64(header + 44);
        segment->skip = decode64(header + 52);
        if (segment->minimum > segment->maximum ||
            (segment->previous != 0 && (segment->previous < 160 || segment->previous >= offset)) ||
            (segment->skip != 0 && (segment->skip < 160 || segment->skip >= offset)))
        {
            return TIMELITE_INVALID_DATABASE;
        }
    }
    return 0;
}

static uint64_t low_bit(uint64_t n)
{
    return n & (~n + 1);
}

/* Locate an ordinal without allocating an index. Old headers have no links. */
static int segment_at(struct timelite_file *file, uint64_t end,
                       uint64_t generation, uint64_t tail, uint64_t target,
                       uint64_t *offset, struct segment_info *segment)
{
    uint64_t n = generation, current = tail, step;
    int error;
    while (n != 0)
    {
        error = read_segment(file, current, end, segment);
        if (error != 0)
        {
            return error;
        }
        if (n == target)
        {
            *offset = current;
            return 0;
        }
        if (segment->header_size == 32)
        {
            current = TIMELITE_DATA_START;
            for (n = 1; n < target; n++)
            {
                error = read_segment(file, current, end, segment);
                if (error != 0)
                {
                    return error;
                }
                current += segment->header_size + segment->body;
            }
            error = read_segment(file, current, end, segment);
            *offset = current;
            return error;
        }
        step = low_bit(n);
        if (n - step >= target)
        {
            current = segment->skip;
            n -= step;
        }
        else
        {
            current = segment->previous;
            n--;
        }
    }
    return TIMELITE_INVALID_DATABASE;
}

static void encode_segment(unsigned char *s, uint64_t first, uint64_t last,
                           uint32_t body, uint64_t minimum, uint64_t maximum,
                           uint64_t previous, uint64_t skip, int ordered)
{
    memset(s, 0, 64);
    memcpy(s, ordered ? "TLSPAN07" : "TLUNOR07", 8);
    encode64(s + 8, first);
    encode64(s + 16, last);
    encode32(s + 24, body);
    encode64(s + 28, minimum);
    encode64(s + 36, maximum);
    encode64(s + 44, previous);
    encode64(s + 52, skip);
    encode32(s + 60, checksum(s, 60));
}

static int create_header(struct timelite_file *file, const char *magic,
                         const unsigned char *identity, int database)
{
    unsigned char header[160];
    memset(header, 0, sizeof(header));
    memcpy(header, magic, 8);
    encode32(header + 8, 3);
    memcpy(header + 12, identity, 16);
    encode32(header + 28, checksum(header, 28));
    if (database)
    {
        encode_manifest(header + 32, 0, TIMELITE_DATA_START, 0, 0, 0);
    }
    return write_exact(file, 0, header, database ? sizeof(header) : 32);
}

/* memcpy is used only after checking the native representation. */
static uint64_t float_bits(const double *value)
{
    uint64_t bits;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

int timelite_float64_supported(void)
{
#if defined(__FAST_MATH__) || defined(TIMELITE_DISABLE_FLOAT64) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0) || \
    FLT_RADIX != 2 || DBL_MANT_DIG != 53 || DBL_MIN_EXP != -1021 || DBL_MAX_EXP != 1024
    return 0;
#else
    const double values[] = {1.0, -0.0, -2.5, 0x1p-1074, 0x1.fffffffffffffp1023};
    const uint64_t bits[] = {UINT64_C(0x3ff0000000000000), UINT64_C(0x8000000000000000),
        UINT64_C(0xc004000000000000), UINT64_C(1), UINT64_C(0x7fefffffffffffff)};
    size_t i;
    if (sizeof(double) != 8)
    {
        return 0;
    }
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        if (float_bits(values + i) != bits[i])
        {
            return 0;
        }
    }
    return 1;
#endif
}

static size_t record_size(const void *frame)
{
    return decode32((const unsigned char *)frame + 24) == 15 ? 24 : 20;
}

static int record_type(const unsigned char *record, size_t stride)
{
    return stride == 20 ? 0 : (int)decode32(record + 20);
}

static int finite_bits(uint64_t bits)
{
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

static int check_frame_header(const unsigned char *bytes)
{
    uint32_t count = decode32(bytes + 16);
    return memcmp(bytes, "TLBATCH!", 8) == 0 && count != 0 &&
           count <= TIMELITE_MAX_RECORDS &&
           decode32(bytes + 20) == 32 + count * record_size(bytes) &&
           (decode32(bytes + 24) == 0 || decode32(bytes + 24) == 15) &&
           checksum(bytes, 28) == decode32(bytes + 28);
}

/* END means a validated header establishes an incomplete final frame. A short
 * header is ambiguous and is deliberately not repaired. No outputs on error. */
static int read_frame(struct timelite_file *file, uint64_t offset, uint64_t end,
                       uint64_t sequence, unsigned char *bytes, size_t *length,
                       size_t *records)
{
    uint32_t count, body_length;
    unsigned char *commit;
    size_t total;
    int error;
    if (end - offset < 32)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    error = read_exact(file, offset, bytes, 32);
    if (error != 0)
    {
        return error;
    }
    if (!check_frame_header(bytes) || decode64(bytes + 8) != sequence)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    count = decode32(bytes + 16);
    body_length = decode32(bytes + 20);
    total = (size_t)body_length + 32;
    if ((uint64_t)total > end - offset)
    {
        return TIMELITE_END;
    }
    error = read_exact(file, offset + 32, bytes + 32, total - 32);
    if (error != 0)
    {
        return error;
    }
    commit = bytes + body_length;
    if (memcmp(commit, "TLCOMMIT", 8) != 0 ||
        decode64(commit + 8) != sequence || decode32(commit + 16) != count ||
        decode32(commit + 20) != body_length ||
        decode32(commit + 24) != checksum(bytes, body_length) ||
        decode32(commit + 28) != checksum(commit, 28))
    {
        return TIMELITE_INVALID_DATABASE;
    }
    if (record_size(bytes) == 24)
    {
        size_t i;
        for (i = 0; i < count; i++)
        {
            const unsigned char *record = bytes + 32 + i * 24;
            uint32_t type = decode32(record + 20);
            if (type > 1 || (type == 1 && !finite_bits(decode64(record + 12))))
            {
                return TIMELITE_INVALID_DATABASE;
            }
        }
    }
    *length = total;
    *records = count;
    return 0;
}

int timelite_batches_close(struct timelite_batches *db)
{
    struct timelite_file database, wal;
    int error = 0, second;
    if (db == NULL)
    {
        return EINVAL;
    }
    database = batch_file(db, 0);
    wal = batch_file(db, 1);
    if (is_closed(database) && is_closed(wal))
    {
        return EBADF;
    }
    if (!is_closed(wal))
    {
        error = timelite_file_close(&wal);
    }
    if (!is_closed(database))
    {
        second = timelite_file_close(&database);
        if (error == 0)
        {
            error = second;
        }
    }
    (void)timelite_batches_init(db);
    return error;
}

/* Choose the newest valid manifest. A 32-byte database is the feature 004
 * layout with nothing installed. Without any valid manifest, a file of at most
 * 160 bytes is empty (an install always leaves more), anything larger fails. */
static int read_manifests(struct timelite_file *database, uint64_t size, int *legacy,
                          uint64_t *generation, uint64_t *data_end, uint64_t *last,
                          uint64_t *last_time, uint64_t *tail, int *has_time,
                          struct retention_state *state)
{
    unsigned char slot[64];
    uint64_t g, d, l, revision;
    size_t count;
    int index, found = 0, error;
    memset(state, 0, sizeof(*state));
    state->root = 160;
    *legacy = size == 32;
    *generation = 0;
    *data_end = TIMELITE_DATA_START;
    *last = 0;
    if (*legacy)
    {
        return 0;
    }
    for (index = 0; index < 2; index++)
    {
        error = timelite_file_read(database, 32 + 64 * (uint64_t)index, slot,
                                   sizeof(slot), &count);
        if (error != 0)
        {
            return error;
        }
        if (count != sizeof(slot))
        {
            continue;
        }
        revision = decode64(slot + 8);
        if ((revision & 1) != (uint64_t)index)
        {
            continue;
        }
        if (retention_manifest(slot, size))
        {
            g = decode32(slot + 52);
            d = decode64(slot + 16);
            l = decode64(slot + 24);
        }
        else if (!check_manifest(slot, size, &g, &d, &l))
        {
            continue;
        }
        if (!found || revision > state->revision)
        {
            *generation = g;
            *data_end = d;
            *last = l;
            *last_time = decode64(slot + 32);
            state->enabled = memcmp(slot, "TLRETAIN", 8) == 0;
            state->revision = revision;
            state->root = state->enabled ? decode32(slot + 44) : 160;
            state->phase = state->enabled ? decode32(slot + 56) : 0;
            *tail = state->enabled ? decode32(slot + 40) : decode64(slot + 40);
            *has_time = state->enabled || decode32(slot + 48) == 7;
        }
        found = 1;
    }
    if (!found && size > TIMELITE_DATA_START)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    return 0;
}

/* Validate links while walking headers. Each table entry holds the last
 * offset at a multiple of 2^bit; 24 entries cover the 1 GiB capacity. */
static int check_segments(struct timelite_file *database, uint64_t generation,
                          uint64_t data_end, uint64_t last, uint64_t *tail,
                          uint64_t *last_time, int has_time, uint64_t *span_start,
                          void *scratch, const struct retention_state *state,
                          uint64_t *live)
{
    struct segment_info segment;
    uint64_t offsets[24] = {0};
    uint64_t offset = state->root, previous = 0, sequence = 0, n;
    uint64_t prior_max = 0, frame, frame_sequence;
    size_t length, records;
    unsigned int bit, j;
    int error;
    *live = 0;
    *span_start = 1;
    for (n = 1; n <= generation; n++)
    {
        error = read_segment(database, offset, data_end, &segment);
        if (error != 0)
        {
            return error;
        }
        if (sequence == UINT64_MAX || segment.first <= sequence ||
            (!state->enabled && segment.first != sequence + 1))
        {
            return TIMELITE_INVALID_DATABASE;
        }
        for (bit = 0; (UINT64_C(1) << bit) != low_bit(n); bit++)
        {
            /* The manifest capacity check bounds bit below 24. */
        }
        if (segment.header_size == 64)
        {
            if (segment.previous != previous || segment.skip != offsets[bit])
            {
                return TIMELITE_INVALID_DATABASE;
            }
            if (!segment.ordered)
            {
                *span_start = n + 1;
                prior_max = 0;
            }
            else if (segment.minimum < prior_max)
            {
                *span_start = n;
            }
            prior_max = segment.maximum;
        }
        else
        {
            *span_start = n + 1;
            prior_max = 0;
        }
        for (j = 0; j <= bit; j++)
        {
            offsets[j] = offset;
        }
        previous = offset;
        *live += segment.last - segment.first + 1;
        sequence = segment.last;
        offset += segment.header_size + segment.body;
    }
    if (offset != data_end || sequence > last ||
        (!state->enabled && sequence != last) || (has_time && *tail != previous))
    {
        return TIMELITE_INVALID_DATABASE;
    }
    *tail = previous;
    if (generation != 0)
    {
        if (segment.ordered && !state->enabled)
        {
            if (has_time && *last_time != segment.maximum)
            {
                return TIMELITE_INVALID_DATABASE;
            }
            *last_time = segment.maximum;
        }
        else if (!has_time)
        {
            /* Only legacy final segments lack a persisted last timestamp. */
            frame = previous + segment.header_size;
            frame_sequence = segment.first;
            while (frame < data_end)
            {
                error = read_frame(database, frame, data_end, frame_sequence,
                                   scratch, &length, &records);
                if (error != 0)
                {
                    return error == TIMELITE_END ? TIMELITE_INVALID_DATABASE : error;
                }
                *last_time = decode64((unsigned char *)scratch + 36 + (records - 1) * record_size(scratch));
                frame += length;
                frame_sequence++;
            }
        }
    }
    return 0;
}

/* Legacy spans are unknown; inspect every validated record before deleting. */
static int segment_expires(struct timelite_file *file, uint64_t offset,
                            const struct segment_info *segment, uint64_t cutoff,
                            void *scratch, int *expires)
{
    uint64_t position = offset + segment->header_size;
    uint64_t end = position + segment->body, sequence = segment->first;
    size_t length, records, i;
    int error;
    *expires = segment->maximum < cutoff;
    if (segment->header_size == 64 || cutoff == 0)
    {
        return 0;
    }
    *expires = 1;
    while (position < end)
    {
        error = read_frame(file, position, end, sequence, scratch, &length, &records);
        if (error != 0)
        {
            return error == TIMELITE_END ? TIMELITE_INVALID_DATABASE : error;
        }
        for (i = 0; i < records; i++)
        {
            if (decode64((unsigned char *)scratch + 36 + i * record_size(scratch)) >= cutoff)
            {
                *expires = 0;
            }
        }
        position += length;
        sequence++;
    }
    return sequence - 1 == segment->last ? 0 : TIMELITE_INVALID_DATABASE;
}

/* Source and destination extents never overlap. Preserve legacy header sizes
 * so a retained copy cannot be larger than its original set. */
static int copy_segments(struct timelite_file *file, uint64_t root, uint64_t end,
                           uint64_t destination, int filter, uint64_t cutoff,
                           void *scratch, uint64_t *new_end, uint64_t *new_tail)
{
    struct segment_info segment;
    unsigned char header[64];
    uint64_t offsets[24] = {0};
    uint64_t offset = root, previous = 0, n = 0, position, bound, sequence;
    size_t length, records;
    unsigned int bit, j;
    int error, expires;
    while (offset < end)
    {
        error = read_segment(file, offset, end, &segment);
        if (error != 0)
        {
            return error;
        }
        expires = 0;
        if (filter)
        {
            error = segment_expires(file, offset, &segment, cutoff, scratch, &expires);
            if (error != 0)
            {
                return error;
            }
        }
        if (!expires)
        {
            n++;
            for (bit = 0; (UINT64_C(1) << bit) != low_bit(n); bit++)
            {
                /* Capacity bounds this table to 24 entries. */
            }
            error = read_exact(file, offset, header, segment.header_size);
            if (error != 0)
            {
                return error;
            }
            if (segment.header_size == 64)
            {
                encode64(header + 44, previous);
                encode64(header + 52, offsets[bit]);
                encode32(header + 60, checksum(header, 60));
            }
            error = write_exact(file, destination, header, segment.header_size);
            if (error != 0)
            {
                return error;
            }
            for (j = 0; j <= bit; j++)
            {
                offsets[j] = destination;
            }
            previous = destination;
            destination += segment.header_size;
            position = offset + segment.header_size;
            bound = position + segment.body;
            sequence = segment.first;
            while (position < bound)
            {
                error = read_frame(file, position, bound, sequence, scratch, &length, &records);
                if (error != 0)
                {
                    return error == TIMELITE_END ? TIMELITE_INVALID_DATABASE : error;
                }
                error = write_exact(file, destination, scratch, length);
                if (error != 0)
                {
                    return error;
                }
                position += length;
                destination += length;
                sequence++;
            }
            if (sequence - 1 != segment.last)
            {
                return TIMELITE_INVALID_DATABASE;
            }
        }
        offset += segment.header_size + segment.body;
    }
    *new_end = destination;
    *new_tail = previous;
    return timelite_file_sync(file);
}

static int finish_retention(struct timelite_file *file, struct retention_state *state,
                             uint64_t count, uint64_t *end, uint64_t last,
                             uint64_t time, uint64_t *tail, void *scratch)
{
    uint64_t new_end, new_tail;
    int error;
    if (state->revision > UINT64_MAX - 2)
    {
        return EOVERFLOW;
    }
    if (state->phase == 2)
    {
        error = copy_segments(file, state->root, *end, 160, 0, 0,
                              scratch, &new_end, &new_tail);
        if (error == 0)
        {
            error = publish_retention(file, state, count, new_end, last, time,
                                       new_tail, 160, 3);
        }
        if (error != 0)
        {
            return error;
        }
        *end = new_end;
        *tail = new_tail;
    }
    /* Keep an empty retained database distinguishable from legacy empty files. */
    error = timelite_file_truncate(file, *end == 160 ? 161 : *end);
    if (error == 0)
    {
        error = timelite_file_sync(file);
    }
    if (error == 0)
    {
        error = publish_retention(file, state, count, *end, last, time, *tail, 160, 0);
    }
    return error;
}

int timelite_batches_open(struct timelite_batches *db, const char *database_path,
                          const char *wal_path, enum timelite_open_mode mode,
                          void *scratch, size_t scratch_size)
{
    struct timelite_file database = TIMELITE_FILE_INIT;
    struct timelite_file wal = TIMELITE_FILE_INIT;
    unsigned char identity[16], header[32], wal_header[32];
    uint64_t size = 0, database_size = 0, offset = 32, sequence = 0, first;
    uint64_t generation = 0, data_end = TIMELITE_DATA_START, installed = 0;
    size_t length, records;
    uint64_t last_time = 0, tail = 0, span_start = 1, wal_first = 0, wal_last = 0;
    uint64_t installed_time, wal_minimum = 0, wal_maximum = 0, live = 0;
    struct retention_state retention;
    size_t i;
    int has_time = 0, wal_ordered = 1;
    int create = 0, legacy = 0, stale = 0;
    int error;
    if (db == NULL || database_path == NULL || wal_path == NULL ||
        database_path[0] == '\0' || wal_path[0] == '\0' ||
        strcmp(database_path, wal_path) == 0 || scratch == NULL ||
        (mode != TIMELITE_OPEN_EXISTING && mode != TIMELITE_CREATE_NEW &&
         mode != TIMELITE_OPEN_OR_CREATE))
    {
        return EINVAL;
    }
    if (!is_closed(batch_file(db, 0)) || !is_closed(batch_file(db, 1)))
    {
        return EINVAL;
    }
    if (scratch_size < TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    if (mode == TIMELITE_CREATE_NEW)
    {
        create = 1;
        error = 0;
    }
    else
    {
        error = timelite_file_open(&database, database_path);
        create = error == ENOENT && mode == TIMELITE_OPEN_OR_CREATE;
    }
    if (create)
    {
        /* Never create a replacement DB next to an orphan WAL. */
        error = timelite_file_open(&wal, wal_path);
        if (error == 0)
        {
            error = TIMELITE_PAIR_MISMATCH;
            goto fail;
        }
        if (error != ENOENT)
        {
            goto fail;
        }
        error = timelite_file_identity(identity);
        if (error != 0)
        {
            goto fail;
        }
        error = timelite_file_create(&database, database_path);
        if (error == EEXIST && mode == TIMELITE_OPEN_OR_CREATE)
        {
            /* One bounded attempt to validate the race winner's entire pair. */
            create = 0;
            error = timelite_file_open(&database, database_path);
        }
        else if (error == 0)
        {
            error = timelite_file_lock(&database);
            if (error == 0)
            {
                error = timelite_file_create(&wal, wal_path);
            }
            if (error == 0)
            {
                error = create_header(&database, "TIMELITE", identity, 1);
            }
            if (error == 0)
            {
                error = create_header(&wal, "TIMEWAL!", identity, 0);
            }
        }
    }
    if (error != 0)
    {
        goto fail;
    }
    if (!create)
    {
        error = timelite_file_lock(&database);
        if (error != 0)
        {
            goto fail;
        }
        /* Validate DB before touching its companion (including v1 rejection). */
        error = pair_header(&database, header, "TIMELITE");
        if (error != 0)
        {
            goto fail;
        }
        error = timelite_file_open(&wal, wal_path);
        if (error != 0)
        {
            goto fail;
        }
    }
    error = pair_header(&database, header, "TIMELITE");
    if (error == 0)
    {
        error = pair_header(&wal, wal_header, "TIMEWAL!");
    }
    if (error == 0 && (memcmp(header + 12, wal_header + 12, 16) != 0 ||
                       decode32(header + 8) != decode32(wal_header + 8)))
    {
        error = TIMELITE_PAIR_MISMATCH;
    }
    if (error == 0)
    {
        error = timelite_file_size(&database, &database_size);
    }
    if (error == 0)
    {
        error = read_manifests(&database, database_size, &legacy, &generation,
                               &data_end, &installed, &last_time, &tail, &has_time, &retention);
    }
    if (error == 0)
    {
        error = check_segments(&database, generation, data_end, installed,
                               &tail, &last_time, has_time, &span_start, scratch, &retention, &live);
    }
    if (error == 0)
    {
        error = timelite_file_size(&wal, &size);
        if (error == 0 && (size < 32 || size > TIMELITE_WAL_CAPACITY))
        {
            error = TIMELITE_INVALID_DATABASE;
        }
    }
    if (error != 0)
    {
        goto fail;
    }
    installed_time = last_time;
    sequence = installed;
    if (size > 32)
    {
        /* A first frame at or below the installed sequence is a stale WAL
         * whose reclaim was interrupted; it must hold exactly F..installed. */
        error = read_exact(&wal, 32, scratch, 32);
        if (error == 0 && !check_frame_header(scratch))
        {
            error = TIMELITE_INVALID_DATABASE;
        }
        if (error != 0)
        {
            goto fail;
        }
        first = decode64((unsigned char *)scratch + 8);
        if (first != 0 && first <= installed)
        {
            stale = 1;
            sequence = first - 1;
        }
    }
    while (offset < size)
    {
        error = read_frame(&wal, offset, size, sequence + 1, scratch,
                           &length, &records);
        if (error == TIMELITE_END && !stale)
        {
            break;
        }
        if (error == TIMELITE_END)
        {
            error = TIMELITE_INVALID_DATABASE;
        }
        if (error != 0)
        {
            goto fail;
        }
        for (i = 0; i < records; i++)
        {
            uint64_t time = decode64((unsigned char *)scratch + 36 + i * record_size(scratch));
            if (offset == 32 && i == 0)
            {
                wal_first = wal_minimum = wal_maximum = time;
            }
            else if (time < wal_last)
            {
                wal_ordered = 0;
            }
            if (time < wal_minimum)
            {
                wal_minimum = time;
            }
            if (time > wal_maximum)
            {
                wal_maximum = time;
            }
            wal_last = time;
        }
        offset += length;
        sequence++;
    }
    if (stale && sequence != installed)
    {
        error = TIMELITE_INVALID_DATABASE;
        goto fail;
    }
    if (database_size > data_end && retention.phase == 0 &&
        !(retention.enabled && generation == 0 && database_size == 161) &&
        (stale || sequence == installed))
    {
        /* Bytes beyond data_end come only from an interrupted install, whose
         * frames are still in the WAL. Without such frames the newest manifest
         * was lost after reclaim (or the file was cut): fail closed. */
        error = TIMELITE_INVALID_DATABASE;
        goto fail;
    }
    if (!stale && sequence > installed)
    {
        last_time = wal_last;
    }
    if (stale)
    {
        wal_ordered = 1;
        offset = 32;
    }
    /* Re-provision on every open: visibility is not past durability evidence. */
    error = timelite_file_provision(&database, database_path);
    if (error == 0)
    {
        error = timelite_file_provision(&wal, wal_path);
    }
    if (error == 0 && retention.phase != 0)
    {
        error = finish_retention(&database, &retention, generation, &data_end,
                                 installed, installed_time, &tail, scratch);
    }
    if (error == 0 && offset < size)
    {
        error = timelite_file_truncate(&wal, offset);
        if (error == 0)
        {
            error = timelite_file_sync(&wal);
        }
    }
    if (error != 0)
    {
        goto fail;
    }
    batch_set_file(db, database, 0);
    batch_set_file(db, wal, 1);
    db->private_format = decode32(header + 8);
    db->private_end = offset;
    db->private_sequence = sequence;
    db->private_data_end = data_end;
    db->private_installed = installed;
    db->private_generation = generation;
    db->private_revision = retention.revision;
    db->private_retention = retention.enabled;
    db->private_live_batches = live;
    db->private_last_time = last_time;
    db->private_installed_time = installed_time;
    db->private_last_segment = tail;
    db->private_span_start = span_start;
    db->private_wal_first_time = wal_first;
    db->private_wal_minimum = wal_minimum;
    db->private_wal_maximum = wal_maximum;
    db->private_wal_ordered = wal_ordered;
    db->private_legacy = legacy;
    db->private_failed = 0;
    reset_cursor(db);
    return 0;

fail:
    if (!is_closed(wal))
    {
        (void)timelite_file_close(&wal);
    }
    if (!is_closed(database))
    {
        (void)timelite_file_close(&database);
    }
    return error;
}

static int batch_ready(struct timelite_batches *db)
{
    if (db == NULL)
    {
        return EINVAL;
    }
    if (is_closed(batch_file(db, 1)))
    {
        return EBADF;
    }
    return db->private_failed ? TIMELITE_RECOVERY_REQUIRED : 0;
}

int timelite_batches_get_status(struct timelite_batches *db,
                                struct timelite_batches_status *status)
{
    int error;
    if (status == NULL)
    {
        return EINVAL;
    }
    error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    status->format_version = db->private_format;
    status->committed_batches = db->private_live_batches +
                                (db->private_sequence - db->private_installed);
    status->installed_batches = db->private_live_batches;
    status->pending_batches = db->private_sequence - db->private_installed;
    status->installed_segments = db->private_generation;
    status->wal_bytes = db->private_end;
    status->installed_bytes = db->private_data_end - TIMELITE_DATA_START;
    status->last_timestamp_us = db->private_last_time;
    return 0;
}

static int append_records(struct timelite_batches *db,
                            const struct timelite_record *records,
                            const struct timelite_typed_record *typed, size_t count,
                            void *scratch, size_t scratch_size, uint64_t *sequence)
{
    unsigned char *bytes = scratch;
    unsigned char *commit;
    struct timelite_file wal;
    uint64_t next;
    size_t i, body_length, stride = typed == NULL ? 20 : 24;
    struct timelite_record converted[TIMELITE_MAX_RECORDS];
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if ((records == NULL && typed == NULL) || count == 0 || count > TIMELITE_MAX_RECORDS ||
        scratch == NULL || sequence == NULL)
    {
        return EINVAL;
    }
    if (scratch_size < TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    if (typed != NULL)
    {
        for (i = 0; i < count; i++)
        {
            if (typed[i].value.type != TIMELITE_VALUE_INT64 &&
                typed[i].value.type != TIMELITE_VALUE_FLOAT64)
            {
                return EINVAL;
            }
            if (typed[i].value.type == TIMELITE_VALUE_FLOAT64)
            {
                if (!timelite_float64_supported())
                {
                    return ENOTSUP;
                }
                if (!finite_bits(float_bits(&typed[i].value.data.floating)))
                {
                    return EINVAL;
                }
                if (db->private_format != 3)
                {
                    return TIMELITE_UNSUPPORTED_VERSION;
                }
            }
            converted[i].series = typed[i].series;
            converted[i].timestamp_us = typed[i].timestamp_us;
            converted[i].value = typed[i].value.type == TIMELITE_VALUE_INT64 ?
                                 typed[i].value.data.integer : 0;
        }
        records = converted;
        if (db->private_format == 2)
        {
            stride = 20;
        }
    }
    for (i = 0; i < count; i++)
    {
        if ((i == 0 && db->private_sequence != 0 &&
             records[i].timestamp_us < db->private_last_time) ||
            (i != 0 && records[i].timestamp_us < records[i - 1].timestamp_us))
        {
            return TIMELITE_OUT_OF_ORDER;
        }
    }
    body_length = 32 + count * stride;
    if (body_length + 32 > TIMELITE_WAL_CAPACITY - db->private_end)
    {
        return TIMELITE_WAL_FULL;
    }
    if (db->private_sequence == UINT64_MAX)
    {
        return EOVERFLOW;
    }
    next = db->private_sequence + 1;
    memcpy(bytes, "TLBATCH!", 8);
    encode64(bytes + 8, next);
    encode32(bytes + 16, (uint32_t)count);
    encode32(bytes + 20, (uint32_t)body_length);
    encode32(bytes + 24, stride == 24 ? 15 : 0);
    encode32(bytes + 28, checksum(bytes, 28));
    for (i = 0; i < count; i++)
    {
        unsigned char *record = bytes + 32 + i * record_size(scratch);
        encode32(record, records[i].series);
        encode64(record + 4, records[i].timestamp_us);
        encode64(record + 12, typed != NULL && typed[i].value.type == TIMELITE_VALUE_FLOAT64 ?
                 float_bits(&typed[i].value.data.floating) : (uint64_t)records[i].value);
        if (stride == 24)
        {
            encode32(record + 20, (uint32_t)typed[i].value.type);
        }
    }
    commit = bytes + body_length;
    memcpy(commit, "TLCOMMIT", 8);
    encode64(commit + 8, next);
    encode32(commit + 16, (uint32_t)count);
    encode32(commit + 20, (uint32_t)body_length);
    encode32(commit + 24, checksum(bytes, body_length));
    encode32(commit + 28, checksum(commit, 28));
    wal = batch_file(db, 1);
    /* Set before the first effect; no later failure can accidentally permit retry. */
    db->private_failed = 1;
    error = write_exact(&wal, db->private_end, bytes, body_length);
    if (error == 0)
    {
        error = timelite_file_sync(&wal);
    }
    if (error == 0)
    {
        error = write_exact(&wal, db->private_end + body_length, commit, 32);
    }
    if (error == 0)
    {
        error = timelite_file_sync(&wal);
    }
    if (error != 0)
    {
        return error;
    }
    db->private_failed = 0;
    if (db->private_end == 32)
    {
        db->private_wal_first_time = records[0].timestamp_us;
        db->private_wal_minimum = records[0].timestamp_us;
        db->private_wal_maximum = records[count - 1].timestamp_us;
    }
    if (records[0].timestamp_us < db->private_wal_minimum)
    {
        db->private_wal_minimum = records[0].timestamp_us;
    }
    if (records[count - 1].timestamp_us > db->private_wal_maximum)
    {
        db->private_wal_maximum = records[count - 1].timestamp_us;
    }
    db->private_last_time = records[count - 1].timestamp_us;
    db->private_end += body_length + 32;
    db->private_sequence = next;
    *sequence = next;
    return 0;
}

int timelite_batches_checkpoint(struct timelite_batches *db,
                                void *scratch, size_t scratch_size)
{
    unsigned char header[128];
    struct timelite_file database, wal;
    uint64_t body, data_end, offset = 32, sequence, skip = 0, target, prior_max;
    struct segment_info segment;
    size_t length = 0, records, header_size;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (scratch == NULL)
    {
        return EINVAL;
    }
    if (scratch_size < TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    if (db->private_end == 32)
    {
        return 0;
    }
    prior_max = db->private_installed_time;
    body = db->private_end - 32;
    data_end = db->private_data_end;
    header_size = 64;
    if (db->private_revision == UINT64_MAX)
    {
        return EOVERFLOW;
    }
    if (body + header_size > TIMELITE_DATABASE_CAPACITY - data_end)
    {
        return TIMELITE_DATABASE_FULL;
    }
    database = batch_file(db, 0);
    wal = batch_file(db, 1);
    target = db->private_generation + 1 - low_bit(db->private_generation + 1);
    if (target != 0)
    {
        error = segment_at(&database, data_end, db->private_generation,
                           db->private_last_segment, target, &skip, &segment);
        if (error != 0)
        {
            return error;
        }
    }
    if (db->private_retention && db->private_generation != 0)
    {
        error = read_segment(&database, db->private_last_segment, data_end, &segment);
        if (error != 0)
        {
            return error;
        }
        prior_max = segment.maximum;
    }
    /* Set before the first effect, exactly like append. */
    db->private_failed = 1;
    if (db->private_legacy)
    {
        /* Feature 004 layout: add the manifest slots before the first install. */
        memset(header, 0, sizeof(header));
        encode_manifest(header, 0, TIMELITE_DATA_START, 0, 0, 0);
        error = write_exact(&database, 32, header, sizeof(header));
        if (error == 0)
        {
            error = timelite_file_sync(&database);
        }
        if (error != 0)
        {
            return error;
        }
        db->private_legacy = 0;
    }
    /* Step 1: segment header and re-validated frames, then sync. Nothing
     * references these bytes yet, so an interruption leaves an ignored orphan. */
    encode_segment(header, db->private_installed + 1, db->private_sequence,
                   (uint32_t)body, db->private_wal_minimum, db->private_wal_maximum,
                   db->private_last_segment, skip, db->private_wal_ordered);
    error = write_exact(&database, data_end, header, header_size);
    sequence = db->private_installed;
    while (error == 0 && offset < db->private_end)
    {
        error = read_frame(&wal, offset, db->private_end, sequence + 1, scratch,
                           &length, &records);
        if (error == TIMELITE_END)
        {
            error = TIMELITE_INVALID_DATABASE;
        }
        if (error == 0)
        {
            error = write_exact(&database, data_end + header_size + offset - 32, scratch, length);
        }
        offset += length;
        sequence++;
    }
    if (error == 0 && sequence != db->private_sequence)
    {
        error = TIMELITE_INVALID_DATABASE;
    }
    if (error == 0)
    {
        error = timelite_file_sync(&database);
    }
    /* Step 2: the new manifest goes into the slot not holding the current one;
     * its sync is the install point. */
    if (error == 0)
    {
        if (db->private_retention)
        {
            encode_retention(header, db->private_revision + 1, db->private_generation + 1,
                             data_end + header_size + body, sequence,
                             db->private_last_time, data_end, 160, 0);
        }
        else
        {
            encode_manifest(header, db->private_generation + 1, data_end + header_size + body,
                            sequence, db->private_last_time, data_end);
        }
        error = write_exact(&database, 32 + 64 * ((db->private_revision + 1) & 1), header, 64);
    }
    if (error == 0)
    {
        error = timelite_file_sync(&database);
    }
    if (error != 0)
    {
        return error;
    }
    if (db->private_wal_first_time < prior_max)
    {
        db->private_span_start = db->private_generation + 1;
    }
    db->private_installed_time = db->private_last_time;
    db->private_generation++;
    db->private_revision++;
    db->private_live_batches += sequence - db->private_installed;
    db->private_data_end = data_end + header_size + body;
    db->private_last_segment = data_end;
    if (!db->private_wal_ordered)
    {
        db->private_span_start = db->private_generation + 1;
    }
    db->private_installed = sequence;
    if (db->private_in_wal)
    {
        /* The segment body is a verbatim copy: WAL offset c is now D + 32 + c. */
        db->private_in_wal = 0;
        db->private_cursor = data_end + header_size + db->private_cursor - 32;
        db->private_segment_end = db->private_data_end;
    }
    /* Step 3: reclaim. The WAL frames are redundant now; recovery finishes an
     * interrupted truncation because it recognises the stale sequences. */
    error = timelite_file_truncate(&wal, 32);
    if (error == 0)
    {
        error = timelite_file_sync(&wal);
    }
    if (error != 0)
    {
        return error;
    }
    db->private_end = 32;
    db->private_wal_ordered = 1;
    db->private_failed = 0;
    return 0;
}

int timelite_batches_rewind(struct timelite_batches *db)
{
    int error = batch_ready(db);
    if (error == 0)
    {
        reset_cursor(db);
    }
    return error;
}

/* A borrowed cursor view never closes or changes ownership of native files. */
static int read_cursor(struct timelite_batches *db, void *scratch,
                        size_t *count, uint64_t *sequence)
{
    struct timelite_file file;
    unsigned char *bytes = scratch;
    uint64_t cursor, segment_end, expected = db->private_read_sequence;
    struct segment_info segment;
    size_t length, found;
    int in_wal, error;
    /* Work on copies so any error leaves the cursor exactly where it was. */
    in_wal = db->private_in_wal;
    cursor = db->private_cursor;
    segment_end = db->private_segment_end;
    if (!in_wal && cursor == db->private_data_end)
    {
        in_wal = 1;
        cursor = 32;
        expected = db->private_installed + 1;
    }
    if (in_wal)
    {
        if (cursor == db->private_end)
        {
            return TIMELITE_END;
        }
        file = batch_file(db, 1);
        error = read_frame(&file, cursor, db->private_end, expected,
                           bytes, &length, &found);
    }
    else
    {
        file = batch_file(db, 0);
        if (cursor == segment_end)
        {
            error = read_segment(&file, cursor, db->private_data_end, &segment);
            if (error != 0)
            {
                return error;
            }
            if (segment.first != expected &&
                (!db->private_retention || segment.first < expected))
            {
                return TIMELITE_INVALID_DATABASE;
            }
            expected = segment.first;
            cursor += segment.header_size;
            segment_end = cursor + segment.body;
        }
        /* Installed segments are complete: a frame that does not fit is damage. */
        error = read_frame(&file, cursor, segment_end, expected,
                           bytes, &length, &found);
        if (error == TIMELITE_END)
        {
            error = TIMELITE_INVALID_DATABASE;
        }
    }
    if (error != 0)
    {
        return error;
    }
    *count = found;
    *sequence = expected;
    db->private_in_wal = in_wal;
    db->private_cursor = cursor + length;
    db->private_segment_end = segment_end;
    db->private_read_sequence = expected + 1;
    return 0;
}

static int read_arguments(struct timelite_batches *db, void *scratch,
                           size_t scratch_size)
{
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (scratch == NULL)
    {
        return EINVAL;
    }
    return scratch_size < TIMELITE_BATCH_SCRATCH ? TIMELITE_BUFFER_TOO_SMALL : 0;
}

int timelite_batches_expire_before(struct timelite_batches *db, uint64_t cutoff_us,
                                   void *scratch, size_t scratch_size)
{
    struct timelite_file file;
    struct segment_info segment;
    struct retention_state state;
    struct timelite_batches cursor, next;
    uint64_t offset = 160, bytes = 0, segments = 0, live = 0;
    uint64_t end, tail, wanted, ignored, span;
    size_t records;
    int expires, error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    file = batch_file(db, 0);
    while (offset < db->private_data_end)
    {
        error = read_segment(&file, offset, db->private_data_end, &segment);
        if (error == 0)
        {
            error = segment_expires(&file, offset, &segment, cutoff_us, scratch, &expires);
        }
        if (error != 0)
        {
            return error;
        }
        if (!expires)
        {
            bytes += segment.header_size + segment.body;
            segments++;
            live += segment.last - segment.first + 1;
        }
        offset += segment.header_size + segment.body;
    }
    if (segments == db->private_generation)
    {
        return 0;
    }
    if (bytes > TIMELITE_DATABASE_CAPACITY - db->private_data_end)
    {
        return TIMELITE_DATABASE_FULL;
    }
    if (db->private_revision > UINT64_MAX - 5)
    {
        return EOVERFLOW;
    }
    state.revision = db->private_revision;
    state.root = 160;
    state.phase = 0;
    state.enabled = db->private_retention;
    wanted = db->private_read_sequence;
    db->private_failed = 1;
    error = publish_retention(&file, &state, db->private_generation,
                               db->private_data_end, db->private_installed,
                               db->private_installed_time, db->private_last_segment, 160, 1);
    if (error == 0)
    {
        error = copy_segments(&file, 160, db->private_data_end, db->private_data_end,
                               1, cutoff_us, scratch, &end, &tail);
    }
    if (error == 0)
    {
        error = publish_retention(&file, &state, segments, end, db->private_installed,
                                   db->private_installed_time, tail, db->private_data_end, 2);
    }
    if (error == 0)
    {
        error = finish_retention(&file, &state, segments, &end, db->private_installed,
                                  db->private_installed_time, &tail, scratch);
    }
    if (error != 0)
    {
        return error;
    }
    cursor = *db;
    cursor.private_generation = segments;
    cursor.private_revision = state.revision;
    cursor.private_retention = 1;
    cursor.private_live_batches = live;
    cursor.private_data_end = end;
    cursor.private_last_segment = tail;
    error = check_segments(&file, segments, end, cursor.private_installed, &tail,
                             &cursor.private_installed_time, 1, &span, scratch, &state, &live);
    if (error != 0)
    {
        return error;
    }
    cursor.private_span_start = span;
    reset_cursor(&cursor);
    /* Locate the original next sequence, or the first surviving successor.
     * Borrowed cursors never acquire or release file ownership. */
    for (;;)
    {
        if (wanted == 0)
        {
            error = TIMELITE_END;
            break;
        }
        next = cursor;
        error = read_cursor(&next, scratch, &records, &ignored);
        if (error == TIMELITE_END || (error == 0 && ignored >= wanted))
        {
            break;
        }
        if (error != 0)
        {
            return error;
        }
        cursor = next;
    }
    if (error == TIMELITE_END)
    {
        cursor.private_in_wal = 1;
        cursor.private_cursor = cursor.private_end;
        cursor.private_read_sequence = cursor.private_sequence + 1;
    }
    cursor.private_failed = 0;
    *db = cursor;
    return 0;
}

static void copy_cursor(struct timelite_batches *destination,
                         const struct timelite_batches *source)
{
    destination->private_cursor = source->private_cursor;
    destination->private_segment_end = source->private_segment_end;
    destination->private_read_sequence = source->private_read_sequence;
    destination->private_in_wal = source->private_in_wal;
}

static void decode_record(const unsigned char *record, size_t stride,
                           void *output, size_t index, int typed)
{
    uint64_t bits = decode64(record + 12);
    int64_t integer = bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
    if (typed)
    {
        struct timelite_typed_record *out = (struct timelite_typed_record *)output + index;
        out->series = decode32(record);
        out->timestamp_us = decode64(record + 4);
        out->value.type = (enum timelite_value_type)record_type(record, stride);
        if (out->value.type == TIMELITE_VALUE_FLOAT64)
        {
            memcpy(&out->value.data.floating, &bits, sizeof(bits));
        }
        else
        {
            out->value.data.integer = integer;
        }
    }
    else
    {
        struct timelite_record *out = (struct timelite_record *)output + index;
        out->series = decode32(record);
        out->timestamp_us = decode64(record + 4);
        out->value = integer;
    }
}

static int output_type(const unsigned char *record, size_t stride, int typed)
{
    if (record_type(record, stride) == 1)
    {
        if (!typed)
        {
            return TIMELITE_VALUE_TYPE_MISMATCH;
        }
        if (!timelite_float64_supported())
        {
            return ENOTSUP;
        }
    }
    return 0;
}

static int next_records(struct timelite_batches *db,
                          void *records, int typed, size_t capacity,
                          size_t *count, uint64_t *sequence,
                          void *scratch, size_t scratch_size)
{
    struct timelite_batches cursor;
    size_t found, i;
    uint64_t batch;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if ((records == NULL && capacity != 0) || count == NULL || sequence == NULL || scratch == NULL)
    {
        return EINVAL;
    }
    error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    cursor = *db;
    error = read_cursor(&cursor, scratch, &found, &batch);
    if (error != 0)
    {
        return error;
    }
    if (capacity < found)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < found; i++)
    {
        error = output_type((unsigned char *)scratch + 32 + i * record_size(scratch),
                            record_size(scratch), typed);
        if (error != 0)
        {
            return error;
        }
    }
    for (i = 0; i < found; i++)
    {
        decode_record((unsigned char *)scratch + 32 + i * record_size(scratch),
                      record_size(scratch), records, i, typed);
    }
    *count = found;
    *sequence = batch;
    copy_cursor(db, &cursor);
    return 0;
}

static void position_segment(struct timelite_batches *cursor, uint64_t offset,
                              const struct segment_info *segment)
{
    cursor->private_cursor = offset + segment->header_size;
    cursor->private_segment_end = cursor->private_cursor + segment->body;
    cursor->private_read_sequence = segment->first;
    cursor->private_in_wal = 0;
}

/* Stop before limit (a sequence), retaining the first qualifying batch. */
static int seek_frames(struct timelite_batches *cursor, uint64_t from_us,
                        uint64_t limit, void *scratch)
{
    struct timelite_batches next;
    uint64_t sequence;
    size_t count;
    int error;
    while (cursor->private_read_sequence <= limit)
    {
        next = *cursor;
        error = read_cursor(&next, scratch, &count, &sequence);
        if (error != 0)
        {
            return error;
        }
        if (decode64((unsigned char *)scratch + 36 + (count - 1) * record_size(scratch)) >= from_us)
        {
            return 0;
        }
        copy_cursor(cursor, &next);
    }
    return TIMELITE_END;
}

int timelite_batches_seek(struct timelite_batches *db, uint64_t from_us,
                          void *scratch, size_t scratch_size)
{
    struct timelite_batches cursor;
    struct timelite_file file;
    struct segment_info segment;
    uint64_t low, high, middle, offset = 160, n;
    int error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    cursor = *db;
    reset_cursor(&cursor);
    file = batch_file(db, 0);
    /* The prefix may be unordered. It cannot be excluded by binary search. */
    for (n = 1; n < db->private_span_start; n++)
    {
        error = read_segment(&file, offset, db->private_data_end, &segment);
        if (error != 0)
        {
            return error;
        }
        position_segment(&cursor, offset, &segment);
        error = seek_frames(&cursor, from_us, segment.last, scratch);
        if (error == 0)
        {
            copy_cursor(db, &cursor);
            return 0;
        }
        if (error != TIMELITE_END)
        {
            return error;
        }
        offset += segment.header_size + segment.body;
    }
    low = db->private_span_start;
    high = db->private_generation + 1;
    while (low < high)
    {
        middle = low + (high - low) / 2;
        error = segment_at(&file, db->private_data_end, db->private_generation,
                           db->private_last_segment, middle, &offset, &segment);
        if (error != 0)
        {
            return error;
        }
        if (segment.maximum < from_us)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low <= db->private_generation)
    {
        error = segment_at(&file, db->private_data_end, db->private_generation,
                           db->private_last_segment, low, &offset, &segment);
        if (error != 0)
        {
            return error;
        }
        position_segment(&cursor, offset, &segment);
    }
    else
    {
        cursor.private_in_wal = 1;
        cursor.private_cursor = 32;
        cursor.private_read_sequence = db->private_installed + 1;
    }
    error = seek_frames(&cursor, from_us, db->private_sequence, scratch);
    if (error == 0 || error == TIMELITE_END)
    {
        if (error == TIMELITE_END)
        {
            cursor.private_in_wal = 1;
            cursor.private_cursor = db->private_end;
            cursor.private_read_sequence = db->private_sequence + 1;
        }
        copy_cursor(db, &cursor);
    }
    return error;
}

static int record_matches(const unsigned char *record, const struct timelite_range *range)
{
    uint64_t time = decode64(record + 4);
    return time >= range->from_us && time < range->until_us &&
           (!range->filter_series || decode32(record) == range->series);
}

static int next_range_records(struct timelite_batches *db,
                                const struct timelite_range *range,
                                void *records, int typed, size_t capacity,
                                size_t *count, uint64_t *sequence,
                                void *scratch, size_t scratch_size)
{
    struct timelite_batches cursor;
    struct timelite_file file;
    struct segment_info segment;
    uint64_t batch, offset, ordered_sequence, first_ordered;
    size_t found, matches, i;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (range == NULL || range->from_us > range->until_us ||
        (range->filter_series != 0 && range->filter_series != 1) ||
        (records == NULL && capacity != 0) || count == NULL || sequence == NULL || scratch == NULL)
    {
        return EINVAL;
    }
    error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    if (range->from_us == range->until_us)
    {
        return TIMELITE_END;
    }
    /* Only a proven ordered suffix permits stopping at the upper bound. An
     * unordered legacy WAL prevents an earlier installed-data stop as well. */
    ordered_sequence = db->private_sequence;
    if (db->private_wal_ordered)
    {
        ordered_sequence = db->private_installed + 1;
        if (db->private_span_start <= db->private_generation)
        {
            file = batch_file(db, 0);
            error = segment_at(&file, db->private_data_end, db->private_generation,
                               db->private_last_segment, db->private_span_start,
                               &offset, &segment);
            if (error != 0)
            {
                return error;
            }
            first_ordered = segment.first;
            if (db->private_retention && db->private_end != 32)
            {
                error = read_segment(&file, db->private_last_segment, db->private_data_end, &segment);
                if (error != 0)
                {
                    return error;
                }
            }
            /* A legacy WAL can start below the installed maximum. */
            if (db->private_end == 32 || db->private_wal_first_time >=
                (db->private_retention ? segment.maximum : db->private_installed_time))
            {
                ordered_sequence = first_ordered;
            }
        }
    }
    cursor = *db;
    for (;;)
    {
        error = read_cursor(&cursor, scratch, &found, &batch);
        if (error != 0)
        {
            return error;
        }
        matches = 0;
        for (i = 0; i < found; i++)
        {
            const unsigned char *record = (unsigned char *)scratch + 32 + i * record_size(scratch);
            if (record_matches(record, range))
            {
                error = output_type(record, record_size(scratch), typed);
                if (error != 0)
                {
                    return error;
                }
                matches++;
            }
        }
        if (matches != 0)
        {
            if (matches > capacity)
            {
                return TIMELITE_BUFFER_TOO_SMALL;
            }
            matches = 0;
            for (i = 0; i < found; i++)
            {
                const unsigned char *record = (unsigned char *)scratch + 32 + i * record_size(scratch);
                if (record_matches(record, range))
                {
                    decode_record(record, record_size(scratch), records, matches++, typed);
                }
            }
            *count = matches;
            *sequence = batch;
            copy_cursor(db, &cursor);
            return 0;
        }
        if (batch >= ordered_sequence && db->private_wal_ordered &&
            decode64((unsigned char *)scratch + 36) >= range->until_us)
        {
            return TIMELITE_END;
        }
    }
}

int timelite_batches_aggregate_range(struct timelite_batches *db,
                                     const struct timelite_range *range,
                                     struct timelite_aggregate *result,
                                     void *scratch, size_t scratch_size)
{
    struct timelite_batches cursor;
    struct timelite_aggregate aggregate = {0, 0, 0};
    struct timelite_record records[TIMELITE_MAX_RECORDS];
    size_t count, i;
    uint64_t sequence;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (range == NULL || range->from_us > range->until_us ||
        (range->filter_series != 0 && range->filter_series != 1) || result == NULL)
    {
        return EINVAL;
    }
    error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    /* Borrow files without acquiring or releasing ownership. Rewind also
     * covers legacy unordered batches that a last-record seek could skip. */
    cursor = *db;
    reset_cursor(&cursor);
    while ((error = timelite_batches_next_range(&cursor, range, records,
             TIMELITE_MAX_RECORDS, &count, &sequence, scratch, scratch_size)) == 0)
    {
        for (i = 0; i < count; i++)
        {
            if (aggregate.record_count == 0 || records[i].value < aggregate.minimum_value)
            {
                aggregate.minimum_value = records[i].value;
            }
            if (aggregate.record_count == 0 || records[i].value > aggregate.maximum_value)
            {
                aggregate.maximum_value = records[i].value;
            }
            /* File capacity bounds the number of records well below UINT64_MAX. */
            aggregate.record_count++;
        }
    }
    if (error != TIMELITE_END)
    {
        return error;
    }
    *result = aggregate;
    return 0;
}

int timelite_batches_append(struct timelite_batches *db,
                            const struct timelite_record *records, size_t count,
                            void *scratch, size_t scratch_size, uint64_t *sequence)
{
    return append_records(db, records, NULL, count, scratch, scratch_size, sequence);
}

int timelite_batches_append_typed(struct timelite_batches *db,
                                  const struct timelite_typed_record *records, size_t count,
                                  void *scratch, size_t scratch_size, uint64_t *sequence)
{
    return append_records(db, NULL, records, count, scratch, scratch_size, sequence);
}

int timelite_batches_next(struct timelite_batches *db,
                          struct timelite_record *records, size_t capacity,
                          size_t *count, uint64_t *sequence, void *scratch, size_t scratch_size)
{
    return next_records(db, records, 0, capacity, count, sequence, scratch, scratch_size);
}

int timelite_batches_next_typed(struct timelite_batches *db,
                                struct timelite_typed_record *records, size_t capacity,
                                size_t *count, uint64_t *sequence, void *scratch, size_t scratch_size)
{
    return next_records(db, records, 1, capacity, count, sequence, scratch, scratch_size);
}

int timelite_batches_next_range(struct timelite_batches *db,
                                const struct timelite_range *range,
                                struct timelite_record *records, size_t capacity,
                                size_t *count, uint64_t *sequence, void *scratch, size_t scratch_size)
{
    return next_range_records(db, range, records, 0, capacity, count, sequence, scratch, scratch_size);
}

int timelite_batches_next_range_typed(struct timelite_batches *db,
                                      const struct timelite_range *range,
                                      struct timelite_typed_record *records, size_t capacity,
                                      size_t *count, uint64_t *sequence, void *scratch, size_t scratch_size)
{
    return next_range_records(db, range, records, 1, capacity, count, sequence, scratch, scratch_size);
}

/* Compare an unsigned integer with the magnitude of a finite binary64.
 * Split the float into its integer part and a nonzero-fraction flag. Shifts
 * are bounded explicitly; no integer is rounded to floating point. */
static int compare_magnitude(uint64_t integer, uint64_t bits)
{
    int exponent = (int)(bits >> 52) - 1023;
    uint64_t significand = (bits & UINT64_C(0xfffffffffffff)) | UINT64_C(0x10000000000000);
    uint64_t whole;
    int fraction = 0;
    if (bits == 0)
    {
        return integer != 0;
    }
    if (exponent < 0)
    {
        return integer == 0 ? -1 : 1;
    }
    if (exponent >= 64)
    {
        return -1;
    }
    if (exponent >= 52)
    {
        whole = significand << (exponent - 52);
    }
    else
    {
        unsigned int shift = (unsigned int)(52 - exponent);
        whole = significand >> shift;
        fraction = (significand & ((UINT64_C(1) << shift) - 1)) != 0;
    }
    if (integer < whole)
    {
        return -1;
    }
    if (integer > whole)
    {
        return 1;
    }
    return fraction ? -1 : 0;
}

static int compare_values(const struct timelite_value *a, const struct timelite_value *b)
{
    uint64_t x, y;
    int negative_a, negative_b, order;
    if (a->type == TIMELITE_VALUE_INT64 && b->type == TIMELITE_VALUE_INT64)
    {
        return (a->data.integer > b->data.integer) - (a->data.integer < b->data.integer);
    }
    if (a->type == TIMELITE_VALUE_FLOAT64 && b->type == TIMELITE_VALUE_INT64)
    {
        return -compare_values(b, a);
    }
    y = float_bits(&b->data.floating);
    negative_b = (int)(y >> 63);
    y &= UINT64_C(0x7fffffffffffffff);
    if (y == 0)
    {
        negative_b = 0;
    }
    if (a->type == TIMELITE_VALUE_INT64)
    {
        negative_a = a->data.integer < 0;
        x = negative_a ? (uint64_t)(-(a->data.integer + 1)) + 1 : (uint64_t)a->data.integer;
    }
    else
    {
        x = float_bits(&a->data.floating);
        negative_a = (int)(x >> 63);
        x &= UINT64_C(0x7fffffffffffffff);
        if (x == 0)
        {
            negative_a = 0;
        }
    }
    if (negative_a != negative_b)
    {
        return negative_a ? -1 : 1;
    }
    order = a->type == TIMELITE_VALUE_INT64 ? compare_magnitude(x, y) : (x > y) - (x < y);
    return negative_a ? -order : order;
}

int timelite_batches_aggregate_range_typed(struct timelite_batches *db,
                                          const struct timelite_range *range,
                                          struct timelite_typed_aggregate *result,
                                          void *scratch, size_t scratch_size)
{
    struct timelite_batches cursor;
    struct timelite_typed_aggregate aggregate;
    struct timelite_typed_record records[TIMELITE_MAX_RECORDS];
    size_t count, i;
    uint64_t sequence;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (range == NULL || range->from_us > range->until_us ||
        (range->filter_series != 0 && range->filter_series != 1) || result == NULL)
    {
        return EINVAL;
    }
    error = read_arguments(db, scratch, scratch_size);
    if (error != 0)
    {
        return error;
    }
    aggregate.record_count = 0;
    aggregate.minimum_value.type = TIMELITE_VALUE_INT64;
    aggregate.minimum_value.data.integer = 0;
    aggregate.maximum_value = aggregate.minimum_value;
    cursor = *db;
    reset_cursor(&cursor);
    while ((error = timelite_batches_next_range_typed(&cursor, range, records,
             TIMELITE_MAX_RECORDS, &count, &sequence, scratch, scratch_size)) == 0)
    {
        for (i = 0; i < count; i++)
        {
            if (aggregate.record_count == 0 || compare_values(&records[i].value, &aggregate.minimum_value) < 0)
            {
                aggregate.minimum_value = records[i].value;
            }
            if (aggregate.record_count == 0 || compare_values(&records[i].value, &aggregate.maximum_value) > 0)
            {
                aggregate.maximum_value = records[i].value;
            }
            aggregate.record_count++;
        }
    }
    if (error != TIMELITE_END)
    {
        return error;
    }
    *result = aggregate;
    return 0;
}
