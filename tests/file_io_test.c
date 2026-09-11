#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)

int main(void)
{
    char directory[] = "/tmp/timelite-io-XXXXXX";
    char path[128];
    char link_path[128];
    struct timelite_file file = TIMELITE_FILE_INIT;
    struct timelite_file other = TIMELITE_FILE_INIT;
    unsigned char buffer[16];
    size_t count = 99;
    uint64_t size = 99;
    uint64_t large_offset = UINT64_C(4294967296) + 17;
    int failed = 0;
    int created = 0;
    int linked = 0;
    int error;

    if (mkdtemp(directory) == NULL)
    {
        perror("mkdtemp");
        return 1;
    }
    (void)snprintf(path, sizeof(path), "%s/data", directory);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", directory);
    CHECK(timelite_file_open(&file, path) == ENOENT);
    CHECK(file.fd == -1);
    CHECK(timelite_file_create(&file, path) == 0);
    created = 1;
    CHECK((fcntl(file.fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(timelite_file_create(&other, path) == EEXIST);
    CHECK(other.fd == -1);
    CHECK(timelite_file_open(&file, path) == EINVAL);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 0);
    CHECK(timelite_file_write(&file, 4, "hello", 5, &count) == 0 && count == 5);
    CHECK(timelite_file_write(&file, 5, "A", 1, &count) == 0 && count == 1);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 9);
    CHECK(timelite_file_read(&file, 0, buffer, sizeof(buffer), &count) == 0);
    CHECK(count == 9 && memcmp(buffer, "\0\0\0\0hAllo", 9) == 0);
    CHECK(timelite_file_read(&file, 6, buffer, 2, &count) == 0);
    CHECK(count == 2 && memcmp(buffer, "ll", 2) == 0);
    CHECK(timelite_file_read(&file, 9, buffer, 1, &count) == 0 && count == 0);
    CHECK(timelite_file_read(&file, 20, buffer, 1, &count) == 0 && count == 0);
    CHECK(timelite_file_read(&file, 0, NULL, 0, &count) == 0 && count == 0);
    CHECK(timelite_file_write(&file, 0, NULL, 0, &count) == 0 && count == 0);
    CHECK(timelite_file_read(&file, 0, NULL, 1, &count) == EINVAL && count == 0);
    CHECK(timelite_file_write(&file, 0, NULL, 1, &count) == EINVAL && count == 0);
    CHECK(timelite_file_read(&file, 0, buffer, 1, NULL) == EINVAL);
    CHECK(timelite_file_write(&file, 0, buffer, 1, NULL) == EINVAL);
    CHECK(timelite_file_read(&file, UINT64_MAX, buffer, 1, &count) == EOVERFLOW);
    CHECK(timelite_file_write(&file, INT64_MAX, buffer, 1, &count) == EOVERFLOW);
    CHECK(timelite_file_read(&file, INT64_MAX, buffer, 1, &count) == EOVERFLOW);
    CHECK(timelite_file_write(&file, UINT64_MAX, buffer, 0, &count) == EOVERFLOW);
#if SIZE_MAX > INT64_MAX
    CHECK(timelite_file_read(&file, 0, buffer, SIZE_MAX, &count) == EOVERFLOW);
    CHECK(timelite_file_write(&file, 0, buffer, SIZE_MAX, &count) == EOVERFLOW);
#endif
    CHECK(timelite_file_truncate(&file, UINT64_MAX) == EOVERFLOW);
    CHECK(timelite_file_size(&file, NULL) == EINVAL);
    CHECK(timelite_file_truncate(&file, 6) == 0);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 6);
    CHECK(timelite_file_truncate(&file, 10) == 0);
    CHECK(timelite_file_read(&file, 6, buffer, 4, &count) == 0 && count == 4);
    CHECK(memcmp(buffer, "\0\0\0\0", 4) == 0);
    CHECK(timelite_file_sync(&file) == 0);
    CHECK(timelite_file_close(&file) == 0 && file.fd == -1);
    CHECK(timelite_file_open(&file, path) == 0);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 10);
    CHECK(timelite_file_read(&file, 4, buffer, 2, &count) == 0);
    CHECK(count == 2 && memcmp(buffer, "hA", 2) == 0);
    CHECK(timelite_file_create(&other, path) == EEXIST);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 10);
    CHECK(symlink(path, link_path) == 0);
    linked = 1;
    CHECK(timelite_file_create(&other, link_path) == EEXIST);
    CHECK(timelite_file_open(&other, link_path) == 0);
    CHECK(timelite_file_close(&other) == 0);
    error = timelite_file_write(&file, large_offset, "Z", 1, &count);
    if (error == EFBIG || error == ENOTSUP)
    {
        printf("SKIP sparse >4 GiB: filesystem error %d\n", error);
    }
    else
    {
        CHECK(error == 0 && count == 1);
        CHECK(timelite_file_size(&file, &size) == 0 && size == large_offset + 1);
        CHECK(timelite_file_close(&file) == 0);
        CHECK(timelite_file_open(&file, path) == 0);
        CHECK(timelite_file_read(&file, large_offset - 1, buffer, 2, &count) == 0);
        CHECK(count == 2 && buffer[0] == 0 && buffer[1] == 'Z');
        CHECK(timelite_file_truncate(&file, large_offset + 8) == 0);
        CHECK(timelite_file_size(&file, &size) == 0 && size == large_offset + 8);
        puts("sparse >4 GiB: passed");
    }
    CHECK(timelite_file_truncate(&file, 0) == 0);
    CHECK(timelite_file_close(&file) == 0);
    CHECK(timelite_file_close(&file) == EBADF);
    CHECK(timelite_file_read(&file, 0, buffer, 1, &count) == EBADF && count == 0);
    CHECK(timelite_file_write(&file, 0, buffer, 1, &count) == EBADF);
    size = 99;
    CHECK(timelite_file_size(&file, &size) == EBADF && size == 99);
    CHECK(timelite_file_sync(&file) == EBADF);
    CHECK(timelite_file_truncate(&file, 0) == EBADF);
    CHECK(timelite_file_open(NULL, path) == EINVAL);
    CHECK(timelite_file_create(&file, NULL) == EINVAL);
    CHECK(timelite_file_open(&file, "") == EINVAL);
    CHECK(timelite_file_close(NULL) == EINVAL);
    CHECK(timelite_file_sync(NULL) == EINVAL);
    CHECK(timelite_file_size(NULL, &size) == EINVAL);
    CHECK(timelite_file_truncate(NULL, 0) == EINVAL);
    CHECK(timelite_file_read(NULL, 0, buffer, 1, &count) == EINVAL);
    CHECK(timelite_file_write(NULL, 0, buffer, 1, &count) == EINVAL);
    CHECK(timelite_file_open(&file, directory) != 0 && file.fd == -1);
    CHECK(unlink(link_path) == 0);
    linked = 0;
    CHECK(mkfifo(link_path, 0600) == 0);
    linked = 1;
    CHECK(timelite_file_open(&file, link_path) == EINVAL && file.fd == -1);

cleanup:
    if (file.fd >= 0 && timelite_file_close(&file) != 0)
    {
        failed = 1;
    }
    if (other.fd >= 0 && timelite_file_close(&other) != 0)
    {
        failed = 1;
    }
    if (linked && unlink(link_path) != 0)
    {
        failed = 1;
    }
    if (created && unlink(path) != 0)
    {
        failed = 1;
    }
    if (rmdir(directory) != 0)
    {
        perror("rmdir");
        failed = 1;
    }
    if (!failed)
    {
        puts("file I/O integration: passed");
    }
    return failed;
}
