#if defined(_WIN32)
/* Use standard C99 functions; test path buffers are checked explicitly. */
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "file_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include "file_io_windows_helpers.h"
#define IS_CLOSED(file) ((file).handle == NULL)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define IS_CLOSED(file) ((file).fd == -1)
#define remove_test_file unlink
#define remove_test_directory rmdir
#endif

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)

int main(void)
{
#if defined(_WIN32)
    char directory[MAX_PATH];
    char path[MAX_PATH];
#else
    char directory[] = "/tmp/timelite-io-XXXXXX";
    char path[128];
    char link_path[128];
#endif
    struct timelite_file file = TIMELITE_FILE_INIT;
    struct timelite_file other = TIMELITE_FILE_INIT;
    unsigned char buffer[16];
    size_t count = 99;
    uint64_t size = 99;
    uint64_t large_offset = UINT64_C(4294967296) + 17;
    int failed = 0;
    int created = 0;
#if !defined(_WIN32)
    int linked = 0;
#endif
    int error;

#if defined(_WIN32)
    if (!make_test_directory(directory, sizeof(directory)))
#else
    if (mkdtemp(directory) == NULL)
#endif
    {
#if defined(_WIN32)
        fprintf(stderr, "temporary directory creation failed: %lu\n", GetLastError());
#else
        perror("mkdtemp");
#endif
        return 1;
    }
#if defined(_WIN32)
    CHECK(snprintf(path, sizeof(path), "%s/data-\xc5\xbe-\xf0\x9f\x95\x92", directory) < (int)sizeof(path));
#else
    (void)snprintf(path, sizeof(path), "%s/data", directory);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", directory);
#endif
    CHECK(timelite_file_open(&file, path) == ENOENT);
    CHECK(IS_CLOSED(file));
    CHECK(timelite_file_create(&file, path) == 0);
    created = 1;
#if defined(_WIN32)
    {
        DWORD flags;
        CHECK(GetHandleInformation(file.handle, &flags));
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
    }
#else
    CHECK((fcntl(file.fd, F_GETFD) & FD_CLOEXEC) != 0);
#endif
    CHECK(timelite_file_create(&other, path) == EEXIST);
    CHECK(IS_CLOSED(other));
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
    CHECK(timelite_file_close(&file) == 0 && IS_CLOSED(file));
    CHECK(timelite_file_open(&file, path) == 0);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 10);
    CHECK(timelite_file_read(&file, 4, buffer, 2, &count) == 0);
    CHECK(count == 2 && memcmp(buffer, "hA", 2) == 0);
    CHECK(timelite_file_create(&other, path) == EEXIST);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 10);
#if !defined(_WIN32)
    CHECK(symlink(path, link_path) == 0);
    linked = 1;
    CHECK(timelite_file_create(&other, link_path) == EEXIST);
    CHECK(timelite_file_open(&other, link_path) == 0);
    CHECK(timelite_file_close(&other) == 0);
#else
    CHECK(timelite_file_open(&other, path) == 0);
    CHECK(timelite_file_close(&other) == 0);
#endif
#if defined(_WIN32)
    error = enable_test_sparse(file.handle);
    CHECK(error >= 0);
    if (error == 0)
    {
        goto sparse_done;
    }
#endif
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
#if defined(_WIN32)
sparse_done:
#endif
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
    CHECK(timelite_file_open(&file, directory) != 0 && IS_CLOSED(file));
#if defined(_WIN32)
    CHECK(check_windows_paths(directory, path));
    CHECK(timelite_file_open(&file, "NUL") == EINVAL && IS_CLOSED(file));
    CHECK(timelite_file_open(&file, path) == 0);
    CHECK(remove_test_file(path) == 0);
    created = 0;
    CHECK(timelite_file_size(&file, &size) == 0 && size == 0);
    CHECK(timelite_file_close(&file) == 0);
#else
    CHECK(unlink(link_path) == 0);
    linked = 0;
    CHECK(mkfifo(link_path, 0600) == 0);
    linked = 1;
    CHECK(timelite_file_open(&file, link_path) == EINVAL && IS_CLOSED(file));

#endif

cleanup:
    if (!IS_CLOSED(file) && timelite_file_close(&file) != 0)
    {
        failed = 1;
    }
    if (!IS_CLOSED(other) && timelite_file_close(&other) != 0)
    {
        failed = 1;
    }
#if !defined(_WIN32)
    if (linked && unlink(link_path) != 0)
    {
        failed = 1;
    }
#endif
    if (created && remove_test_file(path) != 0)
    {
        failed = 1;
    }
    if (remove_test_directory(directory) != 0)
    {
#if defined(_WIN32)
        fprintf(stderr, "temporary directory cleanup failed: %lu\n", GetLastError());
#else
        perror("rmdir");
#endif
        failed = 1;
    }
    if (!failed)
    {
        puts("file I/O integration: passed");
    }
    return failed;
}
