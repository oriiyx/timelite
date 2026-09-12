#include "timelite.h"
#include "file_io.h"

#include <errno.h>
#include <limits.h>
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
    db->private_end = 32;
    db->private_sequence = 0;
    db->private_data_end = TIMELITE_DATA_START;
    db->private_installed = 0;
    db->private_generation = 0;
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
    if (decode32(header + 8) != 2)
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
                            uint64_t data_end, uint64_t last)
{
    memset(m, 0, 64);
    memcpy(m, "TLINSTAL", 8);
    encode64(m + 8, generation);
    encode64(m + 16, data_end);
    encode64(m + 24, last);
    encode32(m + 60, checksum(m, 60));
}

static int check_manifest(const unsigned char *m, uint64_t size,
                          uint64_t *generation, uint64_t *data_end, uint64_t *last)
{
    static const unsigned char zero[28];
    uint64_t g = decode64(m + 8), d = decode64(m + 16), l = decode64(m + 24);
    if (memcmp(m, "TLINSTAL", 8) != 0 || memcmp(m + 32, zero, 28) != 0 ||
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

/* Segment header: the body is a verbatim copy of validated WAL frames. */
static void encode_segment(unsigned char *s, uint64_t first, uint64_t last,
                           uint32_t body_length)
{
    memcpy(s, "TLSEGMNT", 8);
    encode64(s + 8, first);
    encode64(s + 16, last);
    encode32(s + 24, body_length);
    encode32(s + 28, checksum(s, 28));
}

static int check_segment(const unsigned char *s, uint64_t first, uint64_t *last,
                         uint32_t *body_length)
{
    uint64_t l = decode64(s + 16), frames;
    uint32_t body = decode32(s + 24);
    if (memcmp(s, "TLSEGMNT", 8) != 0 || decode64(s + 8) != first || l < first ||
        checksum(s, 28) != decode32(s + 28) || body > TIMELITE_WAL_CAPACITY - 32)
    {
        return 0;
    }
    frames = l - first + 1;
    if (frames > body / 84 || (uint64_t)body > frames * TIMELITE_BATCH_SCRATCH)
    {
        return 0;
    }
    *last = l;
    *body_length = body;
    return 1;
}

static int create_header(struct timelite_file *file, const char *magic,
                         const unsigned char *identity, int database)
{
    unsigned char header[160];
    memset(header, 0, sizeof(header));
    memcpy(header, magic, 8);
    encode32(header + 8, 2);
    memcpy(header + 12, identity, 16);
    encode32(header + 28, checksum(header, 28));
    if (database)
    {
        encode_manifest(header + 32, 0, TIMELITE_DATA_START, 0);
    }
    return write_exact(file, 0, header, database ? sizeof(header) : 32);
}

static int check_frame_header(const unsigned char *bytes)
{
    uint32_t count = decode32(bytes + 16);
    return memcmp(bytes, "TLBATCH!", 8) == 0 && count != 0 &&
           count <= TIMELITE_MAX_RECORDS &&
           decode32(bytes + 20) == 32 + count * 20 && decode32(bytes + 24) == 0 &&
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
                          uint64_t *generation, uint64_t *data_end, uint64_t *last)
{
    unsigned char slot[64];
    uint64_t g, d, l;
    size_t count;
    int index, found = 0, error;
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
        if (count != sizeof(slot) || !check_manifest(slot, size, &g, &d, &l) ||
            (g & 1) != (uint64_t)index)
        {
            continue;
        }
        /* Slot parity makes two valid generations always differ. */
        if (!found || g > *generation)
        {
            *generation = g;
            *data_end = d;
            *last = l;
        }
        found = 1;
    }
    if (!found && size > TIMELITE_DATA_START)
    {
        return TIMELITE_INVALID_DATABASE;
    }
    return 0;
}

/* Walk exactly `generation` segment headers to data_end; frame contents are
 * validated when read, so open costs one small read per segment. */
static int check_segments(struct timelite_file *database, uint64_t generation,
                          uint64_t data_end, uint64_t last)
{
    unsigned char header[32];
    uint64_t offset = TIMELITE_DATA_START, sequence = 0, i;
    uint32_t body;
    int error;
    for (i = 0; i < generation; i++)
    {
        if (data_end - offset < 32)
        {
            return TIMELITE_INVALID_DATABASE;
        }
        error = read_exact(database, offset, header, sizeof(header));
        if (error != 0)
        {
            return error;
        }
        if (!check_segment(header, sequence + 1, &sequence, &body) ||
            body > data_end - offset - 32)
        {
            return TIMELITE_INVALID_DATABASE;
        }
        offset += 32 + body;
    }
    return offset == data_end && sequence == last ? 0 : TIMELITE_INVALID_DATABASE;
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
            error = timelite_file_create(&wal, wal_path);
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
    if (error == 0 && memcmp(header + 12, wal_header + 12, 16) != 0)
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
                               &data_end, &installed);
    }
    if (error == 0)
    {
        error = check_segments(&database, generation, data_end, installed);
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
        offset += length;
        sequence++;
    }
    if (stale && sequence != installed)
    {
        error = TIMELITE_INVALID_DATABASE;
        goto fail;
    }
    if (database_size > data_end && (stale || sequence == installed))
    {
        /* Bytes beyond data_end come only from an interrupted install, whose
         * frames are still in the WAL. Without such frames the newest manifest
         * was lost after reclaim (or the file was cut): fail closed. */
        error = TIMELITE_INVALID_DATABASE;
        goto fail;
    }
    if (stale)
    {
        offset = 32;
    }
    /* Re-provision on every open: visibility is not past durability evidence. */
    error = timelite_file_provision(&database, database_path);
    if (error == 0)
    {
        error = timelite_file_provision(&wal, wal_path);
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
    db->private_end = offset;
    db->private_sequence = sequence;
    db->private_data_end = data_end;
    db->private_installed = installed;
    db->private_generation = generation;
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

int timelite_batches_append(struct timelite_batches *db,
                            const struct timelite_record *records, size_t count,
                            void *scratch, size_t scratch_size, uint64_t *sequence)
{
    unsigned char *bytes = scratch;
    unsigned char *commit;
    struct timelite_file wal;
    uint64_t next;
    size_t i, body_length;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if (records == NULL || count == 0 || count > TIMELITE_MAX_RECORDS ||
        scratch == NULL || sequence == NULL)
    {
        return EINVAL;
    }
    if (scratch_size < TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    body_length = 32 + count * 20;
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
    encode32(bytes + 24, 0);
    encode32(bytes + 28, checksum(bytes, 28));
    for (i = 0; i < count; i++)
    {
        unsigned char *record = bytes + 32 + i * 20;
        encode32(record, records[i].series);
        encode64(record + 4, records[i].timestamp_us);
        encode64(record + 12, (uint64_t)records[i].value);
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
    uint64_t body, data_end, offset = 32, sequence;
    size_t length, records;
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
    body = db->private_end - 32;
    data_end = db->private_data_end;
    if (body + 32 > TIMELITE_DATABASE_CAPACITY - data_end)
    {
        return TIMELITE_DATABASE_FULL;
    }
    database = batch_file(db, 0);
    wal = batch_file(db, 1);
    /* Set before the first effect, exactly like append. */
    db->private_failed = 1;
    if (db->private_legacy)
    {
        /* Feature 004 layout: add the manifest slots before the first install. */
        memset(header, 0, sizeof(header));
        encode_manifest(header, 0, TIMELITE_DATA_START, 0);
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
                   (uint32_t)body);
    error = write_exact(&database, data_end, header, 32);
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
            error = write_exact(&database, data_end + offset, scratch, length);
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
        encode_manifest(header, db->private_generation + 1, data_end + 32 + body,
                        sequence);
        error = write_exact(&database, 32 + 64 * ((db->private_generation + 1) & 1),
                            header, 64);
    }
    if (error == 0)
    {
        error = timelite_file_sync(&database);
    }
    if (error != 0)
    {
        return error;
    }
    db->private_generation++;
    db->private_data_end = data_end + 32 + body;
    db->private_installed = sequence;
    if (db->private_in_wal)
    {
        /* The segment body is a verbatim copy: WAL offset c is now D + c. */
        db->private_in_wal = 0;
        db->private_cursor = data_end + db->private_cursor;
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

int timelite_batches_next(struct timelite_batches *db,
                          struct timelite_record *records, size_t capacity,
                          size_t *count, uint64_t *sequence,
                          void *scratch, size_t scratch_size)
{
    struct timelite_file file;
    unsigned char *bytes = scratch;
    uint64_t cursor, segment_end, last;
    uint32_t body;
    size_t length, found, i;
    int in_wal;
    int error = batch_ready(db);
    if (error != 0)
    {
        return error;
    }
    if ((records == NULL && capacity != 0) || count == NULL || sequence == NULL ||
        scratch == NULL)
    {
        return EINVAL;
    }
    if (scratch_size < TIMELITE_BATCH_SCRATCH)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    /* Work on copies so any error leaves the cursor exactly where it was. */
    in_wal = db->private_in_wal;
    cursor = db->private_cursor;
    segment_end = db->private_segment_end;
    if (!in_wal && cursor == db->private_data_end)
    {
        in_wal = 1;
        cursor = 32;
    }
    if (in_wal)
    {
        if (cursor == db->private_end)
        {
            return TIMELITE_END;
        }
        file = batch_file(db, 1);
        error = read_frame(&file, cursor, db->private_end, db->private_read_sequence,
                           bytes, &length, &found);
    }
    else
    {
        file = batch_file(db, 0);
        if (cursor == segment_end)
        {
            if (db->private_data_end - cursor < 32)
            {
                return TIMELITE_INVALID_DATABASE;
            }
            error = read_exact(&file, cursor, bytes, 32);
            if (error != 0)
            {
                return error;
            }
            if (!check_segment(bytes, db->private_read_sequence, &last, &body) ||
                body > db->private_data_end - cursor - 32)
            {
                return TIMELITE_INVALID_DATABASE;
            }
            cursor += 32;
            segment_end = cursor + body;
        }
        /* Installed segments are complete: a frame that does not fit is damage. */
        error = read_frame(&file, cursor, segment_end, db->private_read_sequence,
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
    if (capacity < found)
    {
        return TIMELITE_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < found; i++)
    {
        const unsigned char *record = bytes + 32 + i * 20;
        uint64_t value = decode64(record + 12);
        records[i].series = decode32(record);
        records[i].timestamp_us = decode64(record + 4);
        /* Avoid implementation-defined unsigned-to-signed conversion. */
        records[i].value = value <= INT64_MAX ? (int64_t)value :
                          -1 - (int64_t)(UINT64_MAX - value);
    }
    *count = found;
    *sequence = db->private_read_sequence;
    db->private_in_wal = in_wal;
    db->private_cursor = cursor + length;
    db->private_segment_end = segment_end;
    db->private_read_sequence++;
    return 0;
}
