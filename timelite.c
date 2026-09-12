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

/* v2 uses 32-byte file headers, batch headers and commit records. */
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
    db->private_cursor = 32;
    db->private_read_sequence = 1;
    db->private_failed = 0;
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

static int create_pair_header(struct timelite_file *file, const char *magic,
                               const unsigned char *identity)
{
    unsigned char header[32];
    memcpy(header, magic, 8);
    encode32(header + 8, 2);
    memcpy(header + 12, identity, 16);
    encode32(header + 28, checksum(header, 28));
    return write_exact(file, 0, header, sizeof(header));
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
    count = decode32(bytes + 16);
    body_length = decode32(bytes + 20);
    if (memcmp(bytes, "TLBATCH!", 8) != 0 || decode64(bytes + 8) != sequence ||
        count == 0 || count > TIMELITE_MAX_RECORDS ||
        body_length != 32 + count * 20 || decode32(bytes + 24) != 0 ||
        checksum(bytes, 28) != decode32(bytes + 28))
    {
        return TIMELITE_INVALID_DATABASE;
    }
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

int timelite_batches_open(struct timelite_batches *db, const char *database_path,
                          const char *wal_path, enum timelite_open_mode mode,
                          void *scratch, size_t scratch_size)
{
    struct timelite_file database = TIMELITE_FILE_INIT;
    struct timelite_file wal = TIMELITE_FILE_INIT;
    unsigned char identity[16], header[32], wal_header[32];
    uint64_t size = 0, offset = 32, sequence = 0;
    size_t length, records;
    int create = 0;
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
                error = create_pair_header(&database, "TIMELITE", identity);
            }
            if (error == 0)
            {
                error = create_pair_header(&wal, "TIMEWAL!", identity);
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
        error = timelite_file_size(&database, &size);
        if (error == 0 && size != 32)
        {
            error = TIMELITE_INVALID_DATABASE;
        }
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
    while (offset < size)
    {
        error = read_frame(&wal, offset, size, sequence + 1, scratch,
                           &length, &records);
        if (error == TIMELITE_END)
        {
            break;
        }
        if (error != 0)
        {
            goto fail;
        }
        offset += length;
        sequence++;
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
    db->private_cursor = 32;
    db->private_read_sequence = 1;
    db->private_failed = 0;
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

int timelite_batches_rewind(struct timelite_batches *db)
{
    int error = batch_ready(db);
    if (error == 0)
    {
        db->private_cursor = 32;
        db->private_read_sequence = 1;
    }
    return error;
}

int timelite_batches_next(struct timelite_batches *db,
                          struct timelite_record *records, size_t capacity,
                          size_t *count, uint64_t *sequence,
                          void *scratch, size_t scratch_size)
{
    struct timelite_file wal;
    unsigned char *bytes = scratch;
    size_t length, found, i;
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
    if (db->private_cursor == db->private_end)
    {
        return TIMELITE_END;
    }
    wal = batch_file(db, 1);
    error = read_frame(&wal, db->private_cursor, db->private_end,
                       db->private_read_sequence, bytes, &length, &found);
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
    db->private_cursor += length;
    db->private_read_sequence++;
    return 0;
}
