#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "file_io.h"
#include "test_assert.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
static int step, fail_step, close_count, swapped, named_calls, unsupported;

static int result(void)
{
    step++;
    if (step == fail_step)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}
int provision_open(const char *path, int flags, ...)
{
    assert(strcmp(path, "/owned") == 0 && (flags & O_DIRECTORY));
    return result() == 0 ? 10 : -1;
}
int provision_fstat(int fd, struct stat *info)
{
    assert(fd == 42);
    memset(info, 0, sizeof(*info));
    info->st_dev = 1;
    info->st_ino = 2;
    info->st_mode = S_IFREG;
    return result();
}
int provision_fstatat(int fd, const char *path, struct stat *info, int flags)
{
    assert(fd == 10 && strcmp(path, "db") == 0 && flags == AT_SYMLINK_NOFOLLOW);
    named_calls++;
    memset(info, 0, sizeof(*info));
    info->st_dev = 1;
    info->st_ino = swapped == named_calls ? 3 : 2;
    info->st_mode = S_IFREG;
    return result();
}
int provision_fstatfs(int fd, struct statfs *info)
{
    assert(fd == 42);
    memset(info, 0, sizeof(*info));
#if defined(__APPLE__)
    strcpy(info->f_fstypename, unsupported ? "other" : "apfs");
#else
    info->f_type = unsupported ? 0 : 0xef53;
#endif
    return result();
}
int provision_fsync(int fd)
{
    assert(fd == 10);
    return result();
}
int provision_close(int fd)
{
    assert(fd == 10);
    close_count++;
    return result();
}
int provision_sync(struct timelite_file *file)
{
    assert(file->fd == 42);
    return result() == 0 ? 0 : EIO;
}
#if defined(__APPLE__)
int provision_entropy(void *buffer, size_t size)
{
    assert(size == 16);
    memset(buffer, 0x45, size);
    return result();
}
#else
ssize_t provision_random(void *buffer, size_t size, unsigned int flags)
{
    (void)flags;
    assert(size == 16);
    memset(buffer, 0x45, size);
    return result() == 0 ? 16 : -1;
}
#endif
int main(void)
{
    struct timelite_file file = {42};
    unsigned char identity[16];
    int count, i;
    assert(timelite_file_provision(&file, "/owned/db") == 0);
    count = step;
    for (i = 1; i <= count; i++)
    {
        TEST_CASE(__func__, i);
        step = close_count = named_calls = 0;
        fail_step = i;
        assert(timelite_file_provision(&file, "/owned/db") == EIO);
        assert(close_count == (i == 1 ? 0 : 1));
        assert(file.fd == 42);
    }
    for (i = 1; i <= 2; i++)
    {
        TEST_CASE(__func__, i);
        step = fail_step = named_calls = 0;
        swapped = i;
        assert(timelite_file_provision(&file, "/owned/db") == EINVAL);
    }
    step = fail_step = named_calls = swapped = 0;
    unsupported = 1;
    assert(timelite_file_provision(&file, "/owned/db") == ENOTSUP);
    step = 0;
    fail_step = 1;
    assert(timelite_file_identity(identity) == EIO);
    step = fail_step = 0;
    assert(timelite_file_identity(identity) == 0 && identity[0] == 0x45);
    puts("native provisioning failures, path replacement and entropy: passed");
    return 0;
}
