/* These must precede all system headers, including on 32-bit Linux. */
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS != 64
#error "file_io.c requires 64-bit file offsets"
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(__linux__) && !defined(__APPLE__)
#error "The file backend currently supports Linux and macOS only"
#endif

#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* A bad toolchain configuration must fail rather than silently limit offsets. */
typedef char timelite_off_t_must_be_signed_64[
    sizeof(off_t) == 8 && (off_t)-1 < 0 ? 1 : -1];

#ifdef TIMELITE_IO_TEST
#include "tests/file_io_calls.h"
#endif

static int check_file(const struct timelite_file *file)
{
    if (file == NULL)
    {
        return EINVAL;
    }
    if (file->fd < 0)
    {
        return EBADF;
    }
    return 0;
}

static int open_file(struct timelite_file *file, const char *path, int flags)
{
    struct stat info;
    int fd;
    int result;
    int error;

    if (file == NULL || path == NULL || path[0] == '\0')
    {
        return EINVAL;
    }
    if (file->fd != -1)
    {
        return EINVAL;
    }
    do
    {
        fd = open(path, flags | O_RDWR | O_CLOEXEC | O_NONBLOCK, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
    {
        return errno;
    }
    do
    {
        result = fstat(fd, &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0 || !S_ISREG(info.st_mode))
    {
        error = result < 0 ? errno : EINVAL;
        /* Preserve the original error. Do not retry a failed close. */
        (void)close(fd);
        return error;
    }
    file->fd = fd;
    return 0;
}

int timelite_file_open(struct timelite_file *file, const char *path)
{
    return open_file(file, path, 0);
}

int timelite_file_create(struct timelite_file *file, const char *path)
{
    return open_file(file, path, O_CREAT | O_EXCL);
}

static int check_transfer(struct timelite_file *file, uint64_t offset,
                          const void *buffer, size_t length, size_t *transferred)
{
    int error;

    if (transferred == NULL)
    {
        return EINVAL;
    }
    *transferred = 0;
    error = check_file(file);
    if (error != 0)
    {
        return error;
    }
    if (buffer == NULL && length != 0)
    {
        return EINVAL;
    }
    if (offset > INT64_MAX || (uintmax_t)length > INT64_MAX - offset)
    {
        return EOVERFLOW;
    }
    return 0;
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *transferred)
{
    int error = check_transfer(file, offset, buffer, length, transferred);
    unsigned char *bytes = buffer;

    if (error != 0)
    {
        return error;
    }
    while (*transferred < length)
    {
        size_t count = length - *transferred;
        ssize_t result;

        if (count > (size_t)SSIZE_MAX)
        {
            count = (size_t)SSIZE_MAX;
        }
        result = pread(file->fd, bytes + *transferred, count,
                       (off_t)(offset + *transferred));
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return errno;
        }
        if (result == 0)
        {
            break;
        }
        *transferred += (size_t)result;
    }
    return 0;
}

int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *transferred)
{
    int error = check_transfer(file, offset, buffer, length, transferred);
    const unsigned char *bytes = buffer;

    if (error != 0)
    {
        return error;
    }
    while (*transferred < length)
    {
        size_t count = length - *transferred;
        ssize_t result;

        if (count > (size_t)SSIZE_MAX)
        {
            count = (size_t)SSIZE_MAX;
        }
        result = pwrite(file->fd, bytes + *transferred, count,
                        (off_t)(offset + *transferred));
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return errno;
        }
        if (result == 0)
        {
            return EIO;
        }
        *transferred += (size_t)result;
    }
    return 0;
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    struct stat info;
    int result;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    if (size == NULL)
    {
        return EINVAL;
    }
    do
    {
        result = fstat(file->fd, &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
    {
        return errno;
    }
    if (info.st_size < 0)
    {
        return EOVERFLOW;
    }
    *size = (uint64_t)info.st_size;
    return 0;
}

int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    int result;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    if (size > INT64_MAX)
    {
        return EOVERFLOW;
    }
    do
    {
        result = ftruncate(file->fd, (off_t)size);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? errno : 0;
}

int timelite_file_sync(struct timelite_file *file)
{
    int result;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    do
    {
#if defined(__APPLE__)
        result = fcntl(file->fd, F_FULLFSYNC);
#else
        result = fdatasync(file->fd);
#endif
    } while (result < 0 && errno == EINTR);
    return result < 0 ? errno : 0;
}

int timelite_file_close(struct timelite_file *file)
{
    int fd;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    fd = file->fd;
    file->fd = -1;
    if (close(fd) < 0)
    {
        return errno;
    }
    return 0;
}

#ifndef TIMELITE_IO_TEST
#include <string.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

#include <sys/random.h>
#ifdef TIMELITE_PROVISION_TEST
#include "tests/provision_calls.h"
#endif

int timelite_file_identity(unsigned char identity[16])
{
#if defined(__APPLE__)
    return getentropy(identity, 16) == 0 ? 0 : errno;
#else
    ssize_t count;
    do
    {
        count = getrandom(identity, 16, GRND_NONBLOCK);
    } while (count < 0 && errno == EINTR);
    return count < 0 ? errno : count == 16 ? 0 : EIO;
#endif
}

int timelite_file_provision(struct timelite_file *file, const char *path)
{
    char parent[4096];
    const char *name;
    const char *slash;
    struct stat opened, named;
    struct statfs filesystem;
    size_t length;
    int directory, result;
    int error = check_file(file);
    if (error != 0 || path == NULL)
    {
        return error != 0 ? error : EINVAL;
    }
    slash = strrchr(path, '/');
    name = slash == NULL ? path : slash + 1;
    length = slash == NULL ? 1 : (size_t)(slash - path);
    if (length == 0)
    {
        length = 1;
    }
    if (length >= sizeof(parent) || name[0] == '\0')
    {
        return ENAMETOOLONG;
    }
    memcpy(parent, slash == NULL ? "." : path, length);
    parent[length] = '\0';
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
    {
        return errno;
    }
    if (fstat(file->fd, &opened) < 0 ||
        fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) < 0 ||
        fstatfs(file->fd, &filesystem) < 0)
    {
        error = errno;
    }
    else if (!S_ISREG(named.st_mode) || opened.st_dev != named.st_dev ||
             opened.st_ino != named.st_ino)
    {
        error = EINVAL;
    }
#if defined(__APPLE__)
    else if (strcmp(filesystem.f_fstypename, "apfs") != 0 &&
             strcmp(filesystem.f_fstypename, "hfs") != 0)
#else
    /* ext4/ext3/ext2 family, XFS and Btrfs; remote and volatile FS excluded. */
    else if ((unsigned long)filesystem.f_type != 0xef53UL && (unsigned long)filesystem.f_type != 0x58465342UL &&
             (unsigned long)filesystem.f_type != 0x9123683eUL)
#endif
    {
        error = ENOTSUP;
    }
    if (error == 0)
    {
        error = timelite_file_sync(file);
    }
    if (error == 0)
    {
        do
        {
            result = fsync(directory);
        } while (result < 0 && errno == EINTR);
        error = result < 0 ? errno : 0;
    }
#if defined(__APPLE__)
    if (error == 0)
    {
        /* Flush device caches after the directory metadata has been submitted. */
        error = timelite_file_sync(file);
    }
#endif
    if (error == 0 && (fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) < 0))
    {
        error = errno;
    }
    if (error == 0 && (!S_ISREG(named.st_mode) || opened.st_dev != named.st_dev ||
                       opened.st_ino != named.st_ino))
    {
        error = EINVAL;
    }
    if (close(directory) < 0 && error == 0)
    {
        error = errno;
    }
    return error;
}
#endif
