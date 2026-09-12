#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "file_io.h"
#include "file_io_windows_calls.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
#define TEST_HANDLE ((HANDLE)(uintptr_t)42)

/* Only this executable has scripted state; native UTF-8 conversion is retained. */
struct step
{
    char operation;
    LONGLONG result;
    DWORD error;
};
static const struct step *steps;
static size_t step_count;
static size_t next_step;
static size_t progress;
static LONGLONG expected_offset;
static DWORD expected_mode = OPEN_EXISTING;
static unsigned char bytes[8];
static int extending;

static void script(const struct step *input, size_t count)
{
    CHECK(next_step == step_count);
    steps = input;
    step_count = count;
    next_step = 0;
    progress = 0;
}

static LONGLONG result_for(char operation)
{
    struct step step;
    CHECK(next_step < step_count);
    step = steps[next_step++];
    CHECK(step.operation == operation);
    SetLastError(step.error);
    return step.result;
}

HANDLE test_create_file(LPCWSTR path, DWORD access, DWORD sharing,
                        LPSECURITY_ATTRIBUTES security, DWORD mode,
                        DWORD flags, HANDLE template_file)
{
    CHECK(wcscmp(path, L"test") == 0);
    CHECK(access == (GENERIC_READ | GENERIC_WRITE));
    CHECK(sharing == (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    CHECK(security == NULL && template_file == NULL);
    CHECK(mode == expected_mode && flags == FILE_ATTRIBUTE_NORMAL);
    return result_for('o') < 0 ? INVALID_HANDLE_VALUE : TEST_HANDLE;
}

DWORD test_file_type(HANDLE handle)
{
    CHECK(handle == TEST_HANDLE);
    return (DWORD)result_for('k');
}

BOOL test_file_info(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info)
{
    LONGLONG result;
    CHECK(handle == TEST_HANDLE);
    result = result_for('i');
    if (result < 0)
    {
        return FALSE;
    }
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = (DWORD)result;
    return TRUE;
}

BOOL test_seek(HANDLE handle, LARGE_INTEGER offset, PLARGE_INTEGER position,
                DWORD method)
{
    CHECK(handle == TEST_HANDLE && offset.QuadPart == expected_offset);
    CHECK(position == NULL && method == FILE_BEGIN);
    return result_for('p') >= 0;
}

BOOL test_read(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD transferred,
                LPOVERLAPPED overlapped)
{
    LONGLONG result;
    CHECK(handle == TEST_HANDLE && overlapped == NULL);
    CHECK(buffer == bytes + progress && count == (5 - progress > 3 ? 3 : 5 - progress));
    result = result_for('r');
    *transferred = 0;
    if (result < 0)
    {
        return FALSE;
    }
    CHECK((DWORD)result <= count);
    memset(buffer, 'x', (size_t)result);
    *transferred = (DWORD)result;
    progress += (size_t)result;
    return TRUE;
}

BOOL test_write(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD transferred,
                 LPOVERLAPPED overlapped)
{
    LONGLONG result;
    CHECK(handle == TEST_HANDLE && overlapped == NULL);
    if (extending)
    {
        CHECK(count == 1 && *(const unsigned char *)buffer == 0);
    }
    else
    {
        CHECK(buffer == bytes + progress && count == (5 - progress > 3 ? 3 : 5 - progress));
    }
    result = result_for('w');
    *transferred = 0;
    if (result < 0)
    {
        return FALSE;
    }
    CHECK((DWORD)result <= count);
    *transferred = (DWORD)result;
    progress += (size_t)result;
    return TRUE;
}

BOOL test_size(HANDLE handle, PLARGE_INTEGER size)
{
    LONGLONG result;
    CHECK(handle == TEST_HANDLE);
    result = result_for('s');
    if (result == -1)
    {
        return FALSE;
    }
    size->QuadPart = result;
    return TRUE;
}

BOOL test_truncate(HANDLE handle)
{
    CHECK(handle == TEST_HANDLE);
    return result_for('t') >= 0;
}

BOOL test_sync(HANDLE handle)
{
    CHECK(handle == TEST_HANDLE);
    return result_for('y') >= 0;
}

BOOL test_close(HANDLE handle)
{
    CHECK(handle == TEST_HANDLE);
    return result_for('c') >= 0;
}

int main(void)
{
    struct timelite_file file = TIMELITE_FILE_INIT;
    size_t count;
    uint64_t size = 99;
    size_t index;
    const struct step open_ok[] = {
        {'o', 0, 0}, {'k', FILE_TYPE_DISK, 0}, {'i', FILE_ATTRIBUTE_NORMAL, 0}};
    const struct step read_short[] = {{'p', 0, 0}, {'r', 2, 0}, {'r', 3, 0}};
    const struct step write_short[] = {{'p', 0, 0}, {'w', 2, 0}, {'w', 3, 0}};
    const struct step read_error[] = {
        {'p', 0, 0}, {'r', 2, 0}, {'r', -1, ERROR_CRC}};
    const struct step write_error[] = {
        {'p', 0, 0}, {'w', 2, 0}, {'w', -1, ERROR_DISK_FULL}};
    const struct step read_eof[] = {{'p', 0, 0}, {'r', 2, 0}, {'r', 0, 0}};
    const struct step native_eof[] = {
        {'p', 0, 0}, {'r', 2, 0}, {'r', -1, ERROR_HANDLE_EOF}};
    const struct step write_zero[] = {{'p', 0, 0}, {'w', 2, 0}, {'w', 0, 0}};
    const struct step seek_error[] = {{'p', -1, ERROR_SEEK}};
    const struct step size_ok[] = {{'s', 123, 0}};
    const struct step size_error[] = {{'s', -1, ERROR_READ_FAULT}};
    const struct step size_negative[] = {{'s', -2, 0}};
    const struct step shrink_ok[] = {{'s', 123, 0}, {'p', 0, 0}, {'t', 0, 0}};
    const struct step shrink_error[] = {
        {'s', 123, 0}, {'p', 0, 0}, {'t', -1, ERROR_DISK_FULL}};
    const struct step shrink_seek_error[] = {{'s', 123, 0}, {'p', -1, ERROR_SEEK}};
    const struct step extend_ok[] = {{'s', 8, 0}, {'p', 0, 0}, {'w', 1, 0}};
    const struct step extend_error[] = {
        {'s', 8, 0}, {'p', 0, 0}, {'w', -1, ERROR_DISK_FULL}};
    const struct step sync_ok[] = {{'y', 0, 0}};
    const struct step sync_error[] = {{'y', -1, ERROR_WRITE_FAULT}};
    const struct step sync_unsupported[] = {{'y', -1, ERROR_NOT_SUPPORTED}};
    const struct step close_error[] = {{'c', -1, ERROR_INVALID_HANDLE}};
    const struct step close_io_error[] = {{'c', -1, ERROR_GEN_FAILURE}};
    const struct step close_ok[] = {{'c', 0, 0}};
    const struct step info_error[] = {
        {'o', 0, 0}, {'k', FILE_TYPE_DISK, 0}, {'i', -1, ERROR_CRC},
        {'c', -1, ERROR_INVALID_HANDLE}};
    const struct step directory[] = {
        {'o', 0, 0}, {'k', FILE_TYPE_DISK, 0}, {'i', FILE_ATTRIBUTE_DIRECTORY, 0},
        {'c', 0, 0}};
    const struct step pipe[] = {{'o', 0, 0}, {'k', FILE_TYPE_PIPE, 0}, {'c', 0, 0}};
    const struct step type_error[] = {
        {'o', 0, 0}, {'k', FILE_TYPE_UNKNOWN, ERROR_ACCESS_DENIED}, {'c', 0, 0}};
    const struct { DWORD native; int mapped; } errors[] = {
        {ERROR_FILE_NOT_FOUND, ENOENT}, {ERROR_PATH_NOT_FOUND, ENOENT},
        {ERROR_FILE_EXISTS, EEXIST}, {ERROR_ALREADY_EXISTS, EEXIST},
        {ERROR_ACCESS_DENIED, EACCES}, {ERROR_SHARING_VIOLATION, EACCES},
        {ERROR_LOCK_VIOLATION, EACCES}, {ERROR_WRITE_PROTECT, EACCES},
        {ERROR_INVALID_HANDLE, EBADF}, {ERROR_INVALID_PARAMETER, EINVAL},
        {ERROR_INVALID_NAME, EINVAL}, {ERROR_BAD_PATHNAME, EINVAL},
        {ERROR_NEGATIVE_SEEK, EINVAL}, {ERROR_ARITHMETIC_OVERFLOW, EOVERFLOW},
        {ERROR_FILE_TOO_LARGE, EFBIG}, {ERROR_DISK_FULL, ENOSPC},
        {ERROR_HANDLE_DISK_FULL, ENOSPC}, {ERROR_NOT_ENOUGH_MEMORY, ENOMEM},
        {ERROR_OUTOFMEMORY, ENOMEM}, {ERROR_TOO_MANY_OPEN_FILES, EMFILE},
        {ERROR_FILENAME_EXCED_RANGE, ENAMETOOLONG},
        {ERROR_INSUFFICIENT_BUFFER, ENAMETOOLONG},
        {ERROR_NO_UNICODE_TRANSLATION, EILSEQ}, {ERROR_NOT_SUPPORTED, ENOTSUP},
        {ERROR_INVALID_FUNCTION, ENOTSUP}, {ERROR_OPERATION_ABORTED, EIO},
        {ERROR_GEN_FAILURE, EIO}};

#define SCRIPT(name) script(name, sizeof(name) / sizeof(name[0]))
    SCRIPT(open_ok);
    CHECK(timelite_file_open(&file, "test") == 0 && file.handle == TEST_HANDLE);
    expected_offset = 10;
    SCRIPT(read_short);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == 0 && count == 5);
    SCRIPT(write_short);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == 0 && count == 5);
    SCRIPT(read_error);
    memset(bytes, 0, sizeof(bytes));
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == EIO && count == 2);
    CHECK(bytes[0] == 'x' && bytes[1] == 'x' && bytes[2] == 0);
    SCRIPT(write_error);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == ENOSPC && count == 2);
    CHECK(file.handle == TEST_HANDLE);
    SCRIPT(read_eof);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == 0 && count == 2);
    SCRIPT(native_eof);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == 0 && count == 2);
    SCRIPT(write_zero);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == EIO && count == 2);
    SCRIPT(seek_error);
    CHECK(timelite_file_read(&file, 10, bytes, 5, &count) == EIO && count == 0);
    SCRIPT(seek_error);
    CHECK(timelite_file_write(&file, 10, bytes, 5, &count) == EIO && count == 0);
    SCRIPT(size_ok);
    CHECK(timelite_file_size(&file, &size) == 0 && size == 123);
    SCRIPT(size_error);
    CHECK(timelite_file_size(&file, &size) == EIO && size == 123);
    SCRIPT(size_negative);
    CHECK(timelite_file_size(&file, &size) == EOVERFLOW && size == 123);
    expected_offset = 12;
    SCRIPT(shrink_ok);
    CHECK(timelite_file_truncate(&file, 12) == 0);
    SCRIPT(shrink_error);
    CHECK(timelite_file_truncate(&file, 12) == ENOSPC);
    SCRIPT(shrink_seek_error);
    CHECK(timelite_file_truncate(&file, 12) == EIO);
    SCRIPT(size_error);
    CHECK(timelite_file_truncate(&file, 12) == EIO);
    expected_offset = 11;
    extending = 1;
    SCRIPT(extend_ok);
    CHECK(timelite_file_truncate(&file, 12) == 0);
    SCRIPT(extend_error);
    CHECK(timelite_file_truncate(&file, 12) == ENOSPC);
    extending = 0;
    SCRIPT(sync_ok);
    CHECK(timelite_file_sync(&file) == 0);
    SCRIPT(sync_error);
    CHECK(timelite_file_sync(&file) == EIO);
    SCRIPT(sync_unsupported);
    CHECK(timelite_file_sync(&file) == ENOTSUP);
    CHECK(file.handle == TEST_HANDLE);
    SCRIPT(close_error);
    CHECK(timelite_file_close(&file) == EBADF && file.handle == NULL);
    CHECK(timelite_file_close(&file) == EBADF);
    SCRIPT(open_ok);
    CHECK(timelite_file_open(&file, "test") == 0);
    SCRIPT(close_io_error);
    CHECK(timelite_file_close(&file) == EIO && file.handle == NULL);
    CHECK(timelite_file_close(&file) == EBADF);
    SCRIPT(info_error);
    CHECK(timelite_file_open(&file, "test") == EIO && file.handle == NULL);
    SCRIPT(directory);
    CHECK(timelite_file_open(&file, "test") == EINVAL && file.handle == NULL);
    SCRIPT(pipe);
    CHECK(timelite_file_open(&file, "test") == EINVAL && file.handle == NULL);
    SCRIPT(type_error);
    CHECK(timelite_file_open(&file, "test") == EACCES && file.handle == NULL);
    expected_mode = CREATE_NEW;
    SCRIPT(open_ok);
    CHECK(timelite_file_create(&file, "test") == 0);
    SCRIPT(close_ok);
    CHECK(timelite_file_close(&file) == 0 && file.handle == NULL);
    for (index = 0; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct step failure = {'o', -1, errors[index].native};
        script(&failure, 1);
        CHECK(timelite_file_create(&file, "test") == errors[index].mapped);
        CHECK(file.handle == NULL);
    }
    CHECK(next_step == step_count);
    puts("Windows file I/O faults: passed");
    return 0;
}
