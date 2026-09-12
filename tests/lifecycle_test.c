#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "timelite.h"
#include "file_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include "windows_test_paths.h"
#else
#include <unistd.h>
#define remove_test_file unlink
#define remove_test_directory rmdir
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); failed = 1; goto cleanup; } } while (0)

int main(void)
{
#if defined(_WIN32)
    char directory[MAX_PATH];
    char path[MAX_PATH];
#else
    char directory[] = "/tmp/timelite-db-XXXXXX";
    char path[128];
#endif
    struct timelite_db db;
    struct timelite_file file = TIMELITE_FILE_INIT;
    const unsigned char expected[12] = {'T','I','M','E','L','I','T','E',1,0,0,0};
    unsigned char data[13];
    unsigned char actual[13];
    size_t count;
    uint64_t size;
    int failed = 0;
    int owned = 0;
    int i;
    int mode;
    int error;

    (void)timelite_init(&db);
#if defined(_WIN32)
    if (!make_test_directory(directory, sizeof(directory)))
#else
    if (mkdtemp(directory) == NULL)
#endif
    {
        return 1;
    }
    CHECK(snprintf(path, sizeof(path), "%s/data-\xc5\xbe-\xf0\x9f\x95\x92", directory) < (int)sizeof(path));
    CHECK(timelite_init(NULL) == EINVAL);
    CHECK(timelite_close(NULL) == EINVAL);
    CHECK(timelite_close(&db) == EBADF);
    CHECK(timelite_open(NULL, path, TIMELITE_CREATE_NEW) == EINVAL);
    CHECK(timelite_open(&db, NULL, TIMELITE_CREATE_NEW) == EINVAL);
    CHECK(timelite_open(&db, "", TIMELITE_CREATE_NEW) == EINVAL);
    CHECK(timelite_open(&db, path, (enum timelite_open_mode)99) == EINVAL);
    CHECK(timelite_open(&db, path, TIMELITE_OPEN_EXISTING) == ENOENT);
    /* Precreate the owned fixture so cleanup remains safe even on test failure. */
    CHECK(timelite_file_create(&file, path) == 0);
    owned = 1;
    CHECK(timelite_file_close(&file) == 0);
    CHECK(remove_test_file(path) == 0);
    CHECK(timelite_open(&db, path, TIMELITE_CREATE_NEW) == 0);
    for (mode = 0; mode < 3; mode++)
    {
        CHECK(timelite_open(&db, path, (enum timelite_open_mode)mode) == EINVAL);
    }
    CHECK(timelite_close(&db) == 0);
    CHECK(timelite_close(&db) == EBADF);
    CHECK(timelite_init(&db) == 0);
    CHECK(timelite_open(&db, path, TIMELITE_CREATE_NEW) == EEXIST);
    CHECK(timelite_open(&db, path, TIMELITE_OPEN_EXISTING) == 0);
    CHECK(timelite_close(&db) == 0);
    CHECK(timelite_open(&db, path, TIMELITE_OPEN_OR_CREATE) == 0);
    CHECK(timelite_close(&db) == 0);
    CHECK(timelite_file_open(&file, path) == 0);
    CHECK(timelite_file_read(&file, 0, actual, sizeof(actual), &count) == 0);
    CHECK(count == 12 && memcmp(actual, expected, 12) == 0);
    CHECK(timelite_file_close(&file) == 0);
    CHECK(remove_test_file(path) == 0);
    CHECK(timelite_open(&db, path, TIMELITE_OPEN_OR_CREATE) == 0);
    CHECK(timelite_close(&db) == 0);
    /* Every interrupted prefix, plus malformed magic, version and trailing byte. */
    for (i = 0; i < 19; i++)
    {
        size_t length = i < 12 ? (size_t)i : 12;
        int expected_error = TIMELITE_INVALID_DATABASE;
        memcpy(data, expected, 12);
        data[12] = 0x7f;
        if (i == 12)
        {
            data[0] = 'X';
        }
        if (i == 13)
        {
            data[8] = 0;
        }
        if (i >= 14 && i <= 17)
        {
            data[8 + i - 14] = 2;
            expected_error = TIMELITE_UNSUPPORTED_VERSION;
        }
        if (i == 18)
        {
            length = 13;
        }
        CHECK(timelite_file_open(&file, path) == 0);
        CHECK(timelite_file_truncate(&file, 0) == 0);
        CHECK(timelite_file_write(&file, 0, data, length, &count) == 0 && count == length);
        CHECK(timelite_file_close(&file) == 0);
        for (mode = 0; mode < 3; mode++)
        {
            error = timelite_open(&db, path, (enum timelite_open_mode)mode);
            CHECK(error == (mode == TIMELITE_CREATE_NEW ? EEXIST : expected_error));
            CHECK(timelite_close(&db) == EBADF);
        }
        CHECK(timelite_file_open(&file, path) == 0);
        CHECK(timelite_file_size(&file, &size) == 0 && size == length);
        CHECK(timelite_file_read(&file, 0, actual, sizeof(actual), &count) == 0);
        CHECK(count == length && memcmp(actual, data, length) == 0);
        CHECK(timelite_file_close(&file) == 0);
    }
cleanup:
    error = timelite_close(&db);
    if (error != 0 && error != EBADF)
    {
        failed = 1;
    }
    error = timelite_file_close(&file);
    if (error != 0 && error != EBADF)
    {
        failed = 1;
    }
    if (owned && remove_test_file(path) != 0)
    {
        failed = 1;
    }
    if (remove_test_directory(directory) != 0)
    {
        failed = 1;
    }
    if (!failed)
    {
        puts("database lifecycle integration: passed");
    }
    return failed;
}
