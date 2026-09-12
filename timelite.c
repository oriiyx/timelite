#include "timelite.h"
#include "file_io.h"

#include <errno.h>
#include <string.h>

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
