#include "windows_test_paths.h"

static int enable_test_sparse(HANDLE handle)
{
    DWORD returned;
    DWORD error;
    if (DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                         &returned, NULL))
    {
        return 1;
    }
    error = GetLastError();
    if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)
    {
        printf("SKIP sparse >4 GiB: FSCTL_SET_SPARSE unsupported (%lu)\n", error);
        return 0;
    }
    fprintf(stderr, "FSCTL_SET_SPARSE failed: %lu\n", error);
    return -1;
}

static int check_windows_paths(const char *directory, const char *path)
{
    struct timelite_file file = TIMELITE_FILE_INIT;
    WCHAR wide[MAX_PATH];
    WCHAR renamed[MAX_PATH];
    char oversized[MAX_PATH + 1];
    HANDLE raw = INVALID_HANDLE_VALUE;
    int failed = 0;
    int moved = 0;
    int readonly = 0;

#define PATH_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Windows path line %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)
    PATH_CHECK(timelite_file_create(&file, "\xc0\xaf") == EILSEQ);
    PATH_CHECK(timelite_file_open(&file, "\xed\xa0\x80") == EILSEQ);
    PATH_CHECK(timelite_file_open(&file, "\xf0\x9f") == EILSEQ);
    PATH_CHECK(file.handle == NULL);
    memset(oversized, 'a', MAX_PATH);
    oversized[MAX_PATH] = '\0';
    PATH_CHECK(timelite_file_create(&file, oversized) == ENAMETOOLONG);
    PATH_CHECK(timelite_file_open(&file, oversized) == ENAMETOOLONG);
    PATH_CHECK(file.handle == NULL);
    /* Independently form the native name, including a surrogate pair. */
    PATH_CHECK(wide_test_path(directory, wide));
    PATH_CHECK(wcslen(wide) + 16 < MAX_PATH);
    wcscat(wide, L"/data-\x017e-\xd83d\xdd52");
    raw = CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    PATH_CHECK(raw != INVALID_HANDLE_VALUE);
    PATH_CHECK(timelite_file_open(&file, path) == EACCES && file.handle == NULL);
    PATH_CHECK(CloseHandle(raw));
    raw = INVALID_HANDLE_VALUE;
    PATH_CHECK(SetFileAttributesW(wide, FILE_ATTRIBUTE_READONLY));
    readonly = 1;
    PATH_CHECK(timelite_file_open(&file, path) == EACCES && file.handle == NULL);
    PATH_CHECK(SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL));
    readonly = 0;
    PATH_CHECK(timelite_file_open(&file, path) == 0);
    PATH_CHECK(wcslen(wide) + 6 < MAX_PATH);
    wcscpy(renamed, wide);
    wcscat(renamed, L"-moved");
    PATH_CHECK(MoveFileW(wide, renamed));
    moved = 1;
    PATH_CHECK(MoveFileW(renamed, wide));
    moved = 0;

cleanup:
    if (raw != INVALID_HANDLE_VALUE && !CloseHandle(raw))
    {
        failed = 1;
    }
    if (file.handle != NULL && timelite_file_close(&file) != 0)
    {
        failed = 1;
    }
    if (readonly && !SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL))
    {
        failed = 1;
    }
    if (moved && !MoveFileW(renamed, wide))
    {
        failed = 1;
    }
#undef PATH_CHECK
    return !failed;
}
