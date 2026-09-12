/* Linked instead of the native backend; production has no hooks or test state. */
#include "timelite.h"
#include "file_io.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned char bytes[12];
static size_t length;
static int exists;
static int open_error, create_error, read_error, write_error, size_error;
static int sync_error, close_error, race;
static size_t read_limit, write_limit;
static int opens, creates, reads, writes, syncs, closes;

static void acquire(struct timelite_file *file)
{
#if defined(_WIN32)
    file->handle = bytes;
#else
    file->fd = 42;
#endif
}

static void reset(int valid)
{
    memcpy(bytes, "TIMELITE\1\0\0\0", 12);
    length = valid ? 12 : 0;
    exists = valid;
    open_error = create_error = read_error = write_error = size_error = 0;
    sync_error = close_error = race = 0;
    read_limit = write_limit = 12;
    opens = creates = reads = writes = syncs = closes = 0;
}

int timelite_file_open(struct timelite_file *file, const char *path)
{
    (void)path;
    opens++;
    if (open_error)
    {
        return open_error;
    }
    if (!exists)
    {
        return ENOENT;
    }
    acquire(file);
    return 0;
}

int timelite_file_create(struct timelite_file *file, const char *path)
{
    (void)path;
    creates++;
    if (race)
    {
        exists = race != 3;
        length = race == 1 ? 12 : 0;
        return EEXIST;
    }
    if (create_error)
    {
        return create_error;
    }
    if (exists)
    {
        return EEXIST;
    }
    exists = 1;
    length = 0;
    acquire(file);
    return 0;
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t capacity, size_t *count)
{
    (void)file;
    assert(offset == 0 && capacity == 12);
    reads++;
    *count = length < read_limit ? length : read_limit;
    memcpy(buffer, bytes, *count);
    return read_error;
}

int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t capacity, size_t *count)
{
    (void)file;
    assert(offset == 0 && capacity == 12);
    writes++;
    *count = write_limit;
    memcpy(bytes, buffer, *count);
    length = *count;
    return write_error;
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    (void)file;
    *size = length;
    return size_error;
}

int timelite_file_sync(struct timelite_file *file)
{
    (void)file;
    syncs++;
    return sync_error;
}

int timelite_file_close(struct timelite_file *file)
{
#if defined(_WIN32)
    if (file->handle == NULL)
    {
        return EBADF;
    }
    file->handle = NULL;
#else
    if (file->fd == -1)
    {
        return EBADF;
    }
    file->fd = -1;
#endif
    closes++;
    return close_error;
}

int main(void)
{
    struct timelite_db db;
    int i;
    assert(timelite_init(&db) == 0);
    for (i = 1; i <= 3; i++)
    {
        reset(0);
        race = i;
        assert(timelite_open(&db, "db", TIMELITE_OPEN_OR_CREATE) ==
               (i == 1 ? 0 : i == 2 ? TIMELITE_INVALID_DATABASE : ENOENT));
        assert(opens == 2 && creates == 1 && writes == 0 && syncs == 0);
        assert(timelite_close(&db) == (i == 1 ? 0 : EBADF));
    }
    reset(0);
    open_error = EACCES;
    assert(timelite_open(&db, "db", TIMELITE_OPEN_OR_CREATE) == EACCES);
    assert(creates == 0 && closes == 0);
    reset(0);
    create_error = ENOSPC;
    assert(timelite_open(&db, "db", TIMELITE_OPEN_OR_CREATE) == ENOSPC);
    assert(writes == 0 && closes == 0);
    for (i = 0; i < 12; i++)
    {
        reset(0);
        write_limit = (size_t)i;
        write_error = i % 2 ? ENOSPC : 0;
        close_error = EACCES;
        assert(timelite_open(&db, "db", TIMELITE_CREATE_NEW) == (i % 2 ? ENOSPC : EIO));
        assert(closes == 1 && syncs == 0 && exists && length == (size_t)i);
        assert(timelite_close(&db) == EBADF && closes == 1);
        assert(timelite_open(&db, "db", TIMELITE_OPEN_OR_CREATE) == TIMELITE_INVALID_DATABASE);
        assert(writes == 1 && length == (size_t)i);
    }
    reset(0);
    sync_error = EIO;
    close_error = EACCES;
    assert(timelite_open(&db, "db", TIMELITE_CREATE_NEW) == EIO);
    assert(closes == 1 && syncs == 1 && length == 12);
    assert(timelite_open(&db, "db", TIMELITE_OPEN_EXISTING) == 0);
    assert(timelite_close(&db) == EACCES);
    assert(timelite_close(&db) == EBADF && closes == 2);
    close_error = 0;
    assert(timelite_open(&db, "db", TIMELITE_OPEN_EXISTING) == 0);
    assert(timelite_close(&db) == 0);
    reset(0);
    write_error = ENOSPC; /* Complete bytes despite failed write. */
    assert(timelite_open(&db, "db", TIMELITE_CREATE_NEW) == ENOSPC);
    assert(syncs == 0 && closes == 1);
    assert(timelite_open(&db, "db", TIMELITE_OPEN_EXISTING) == 0);
    assert(timelite_close(&db) == 0);
    for (i = 0; i < 14; i++)
    {
        reset(1);
        close_error = EACCES;
        if (i < 12)
        {
            read_limit = (size_t)i;
        }
        if (i == 12)
        {
            read_error = EIO;
        }
        if (i == 13)
        {
            size_error = EOVERFLOW;
        }
        assert(timelite_open(&db, "db", TIMELITE_OPEN_EXISTING) ==
               (i < 12 ? TIMELITE_INVALID_DATABASE : i == 12 ? EIO : EOVERFLOW));
        assert(closes == 1 && writes == 0 && syncs == 0);
        assert(timelite_close(&db) == EBADF && closes == 1);
    }
    puts("database lifecycle faults: passed");
    return 0;
}

int timelite_file_identity(unsigned char identity[16])
{
    (void)identity;
    return ENOTSUP;
}
int timelite_file_provision(struct timelite_file *file, const char *path)
{
    (void)file;
    (void)path;
    return ENOTSUP;
}
int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    (void)file;
    (void)size;
    return ENOTSUP;
}
