#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "file_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

/* Scripted syscall results. Test globals are not linked into the library. */
struct step
{
    char operation;
    int result;
    int error;
};
static struct step steps[8];
static size_t step_count;
static size_t next_step;
static size_t progress;
static unsigned char bytes[8];

static void script(const struct step *input, size_t count)
{
    CHECK(next_step == step_count);
    memcpy(steps, input, count * sizeof(*input));
    step_count = count;
    next_step = 0;
    progress = 0;
}

static int result_for(char operation)
{
    struct step step;
    CHECK(next_step < step_count);
    step = steps[next_step++];
    CHECK(step.operation == operation);
    errno = step.error;
    return step.result;
}

int test_open(const char *path, int flags, ...)
{
    CHECK(strcmp(path, "test") == 0);
    CHECK((flags & O_TRUNC) == 0);
    CHECK((flags & O_APPEND) == 0);
    CHECK((flags & O_CLOEXEC) != 0);
    return result_for('o');
}

int test_fstat(int fd, struct stat *info)
{
    int result;
    CHECK(fd == 42);
    result = result_for('s');
    if (result == 0)
    {
        memset(info, 0, sizeof(*info));
        info->st_mode = S_IFREG | 0600;
        info->st_size = 123;
    }
    return result;
}

ssize_t test_pread(int fd, void *buffer, size_t count, off_t offset)
{
    int result;
    CHECK(fd == 42 && offset == (off_t)(10 + progress));
    CHECK(buffer == bytes + progress && count == 5 - progress);
    result = result_for('r');
    if (result > 0)
    {
        CHECK((size_t)result <= count);
        memset(buffer, 'x', (size_t)result);
        progress += (size_t)result;
    }
    return result;
}

ssize_t test_pwrite(int fd, const void *buffer, size_t count, off_t offset)
{
    int result;
    CHECK(fd == 42 && offset == (off_t)(10 + progress));
    CHECK(buffer == bytes + progress && count == 5 - progress);
    result = result_for('w');
    if (result > 0)
    {
        CHECK((size_t)result <= count);
        progress += (size_t)result;
    }
    return result;
}

int test_ftruncate(int fd, off_t size)
{
    CHECK(fd == 42 && size == 12);
    return result_for('t');
}

int test_sync(int fd, ...)
{
    CHECK(fd == 42);
#if defined(__APPLE__)
    {
        va_list arguments;
        va_start(arguments, fd);
        CHECK(va_arg(arguments, int) == F_FULLFSYNC);
        va_end(arguments);
    }
#endif
    return result_for('y');
}

int test_close(int fd)
{
    CHECK(fd == 42);
    return result_for('c');
}

int main(void)
{
    struct timelite_file file = TIMELITE_FILE_INIT;
    size_t count;
    uint64_t size = 99;
    const struct step open_retry[] = {
        {'o', -1, EINTR}, {'o', 42, 0}, {'s', -1, EINTR}, {'s', 0, 0}};
    const struct step read_retry[] = {
        {'r', -1, EINTR}, {'r', 2, 0}, {'r', -1, EINTR}, {'r', 3, 0}};
    const struct step write_retry[] = {
        {'w', -1, EINTR}, {'w', 2, 0}, {'w', -1, EINTR}, {'w', 3, 0}};
    const struct step read_error[] = {{'r', 2, 0}, {'r', -1, EIO}};
    const struct step write_error[] = {{'w', 2, 0}, {'w', -1, ENOSPC}};
    const struct step read_eof[] = {{'r', 2, 0}, {'r', 0, 0}};
    const struct step write_zero[] = {{'w', 2, 0}, {'w', 0, 0}};
    const struct step size_retry[] = {{'s', -1, EINTR}, {'s', 0, 0}};
    const struct step size_error[] = {{'s', -1, EIO}};
    const struct step truncate_retry[] = {{'t', -1, EINTR}, {'t', 0, 0}};
    const struct step truncate_error[] = {{'t', -1, ENOSPC}};
    const struct step sync_retry[] = {{'y', -1, EINTR}, {'y', 0, 0}};
    const struct step sync_error[] = {{'y', -1, EIO}};
    const struct step sync_unsupported[] = {{'y', -1, ENOTSUP}};
    const struct step close_error[] = {{'c', -1, EINTR}};
    const struct step close_io_error[] = {{'c', -1, EIO}};
    const struct step open_error[] = {{'o', -1, EACCES}};
    const struct step stat_error[] = {{'o', 42, 0}, {'s', -1, EIO}, {'c', -1, EBADF}};

#define SCRIPT(name) script(name, sizeof(name) / sizeof(name[0]))
    SCRIPT(open_retry);
    CHECK(timelite_file_open(&file, "test") == 0 && file.fd == 42);
    SCRIPT(read_retry);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == 0 && count == 5);
    SCRIPT(write_retry);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == 0 && count == 5);
    SCRIPT(read_error);
    memset(bytes, 0, sizeof(bytes));
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == EIO && count == 2);
    CHECK(bytes[0] == 'x' && bytes[1] == 'x' && bytes[2] == 0);
    SCRIPT(write_error);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == ENOSPC && count == 2);
    SCRIPT(read_eof);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == 0 && count == 2);
    SCRIPT(write_zero);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == EIO && count == 2);
    SCRIPT(size_retry);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 123);
    SCRIPT(size_error);
    CHECK(timelite_file_size(&file, &size) == EIO && size == 123);
    SCRIPT(truncate_retry);
    CHECK(timelite_file_truncate(&file, 12) == 0);
    SCRIPT(truncate_error);
    CHECK(timelite_file_truncate(&file, 12) == ENOSPC);
    SCRIPT(sync_retry);
    CHECK(timelite_file_sync(&file) == 0);
    SCRIPT(sync_error);
    CHECK(timelite_file_sync(&file) == EIO);
    SCRIPT(sync_unsupported);
    CHECK(timelite_file_sync(&file) == ENOTSUP);
    SCRIPT(close_error);
    CHECK(timelite_file_close(&file) == EINTR && file.fd == -1);
    CHECK(timelite_file_close(&file) == EBADF);
    SCRIPT(open_retry);
    CHECK(timelite_file_open(&file, "test") == 0);
    SCRIPT(close_io_error);
    CHECK(timelite_file_close(&file) == EIO && file.fd == -1);
    SCRIPT(open_error);
    CHECK(timelite_file_create(&file, "test") == EACCES && file.fd == -1);
    SCRIPT(stat_error);
    CHECK(timelite_file_open(&file, "test") == EIO && file.fd == -1);
    CHECK(next_step == step_count);
    puts("file I/O faults: passed");
    return 0;
}
