#if !defined(_WIN32)
#error "Build file_io.c for Linux/macOS instead"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "file_io.h"
#include <errno.h>

#ifdef TIMELITE_IO_TEST
#include "tests/file_io_windows_calls.h"
#endif

#ifndef TIMELITE_IO_TRANSFER_LIMIT
#define TIMELITE_IO_TRANSFER_LIMIT MAXDWORD
#endif

static int windows_error(DWORD error)
{
    switch (error)
    {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ENOENT;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return EEXIST;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return EACCES;
        case ERROR_INVALID_HANDLE:
            return EBADF;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_NEGATIVE_SEEK:
            return EINVAL;
        case ERROR_ARITHMETIC_OVERFLOW:
            return EOVERFLOW;
        case ERROR_FILE_TOO_LARGE:
            return EFBIG;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ENOMEM;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_INSUFFICIENT_BUFFER:
            return ENAMETOOLONG;
        case ERROR_NO_UNICODE_TRANSLATION:
            return EILSEQ;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            return ENOTSUP;
        default:
            return EIO;
    }
}

static int check_file(const struct timelite_file *file)
{
    if (file == NULL)
    {
        return EINVAL;
    }
    if (file->handle == NULL || file->handle == INVALID_HANDLE_VALUE)
    {
        return EBADF;
    }
    return 0;
}

static int open_file(struct timelite_file *file, const char *path, DWORD mode)
{
    WCHAR wide_path[MAX_PATH];
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle;
    size_t length;
    int error = 0;

    if (file == NULL || path == NULL || path[0] == '\0')
    {
        return EINVAL;
    }
    if (file->handle != NULL)
    {
        return EINVAL;
    }
    for (length = 0; length < MAX_PATH && path[length] != '\0'; length++)
    {
    }
    if (length == MAX_PATH)
    {
        return ENAMETOOLONG;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            (int)length + 1, wide_path, MAX_PATH) == 0)
    {
        return windows_error(GetLastError());
    }
    handle = CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, mode, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return windows_error(GetLastError());
    }
    SetLastError(ERROR_SUCCESS);
    if (GetFileType(handle) != FILE_TYPE_DISK)
    {
        DWORD native_error = GetLastError();
        error = native_error == ERROR_SUCCESS ? EINVAL : windows_error(native_error);
    }
    else if (!GetFileInformationByHandle(handle, &info))
    {
        error = windows_error(GetLastError());
    }
    else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        error = EINVAL;
    }
    if (error != 0)
    {
        /* Consume the temporary handle; preserve the validation error. */
        (void)CloseHandle(handle);
        return error;
    }
    file->handle = handle;
    return 0;
}

int timelite_file_open(struct timelite_file *file, const char *path)
{
    return open_file(file, path, OPEN_EXISTING);
}

int timelite_file_create(struct timelite_file *file, const char *path)
{
    return open_file(file, path, CREATE_NEW);
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

static int seek_file(struct timelite_file *file, uint64_t offset)
{
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN))
    {
        return windows_error(GetLastError());
    }
    return 0;
}

int timelite_file_read(struct timelite_file *file, uint64_t offset,
                       void *buffer, size_t length, size_t *transferred)
{
    int error = check_transfer(file, offset, buffer, length, transferred);
    unsigned char *bytes = buffer;

    if (error != 0 || length == 0)
    {
        return error;
    }
    error = seek_file(file, offset);
    if (error != 0)
    {
        return error;
    }
    while (*transferred < length)
    {
        size_t remaining = length - *transferred;
        DWORD count = remaining > TIMELITE_IO_TRANSFER_LIMIT ?
                      TIMELITE_IO_TRANSFER_LIMIT : (DWORD)remaining;
        DWORD result = 0;

        if (!ReadFile(file->handle, bytes + *transferred, count, &result, NULL))
        {
            DWORD native_error = GetLastError();
            return native_error == ERROR_HANDLE_EOF ? 0 : windows_error(native_error);
        }
        if (result == 0)
        {
            break;
        }
        *transferred += result;
    }
    return 0;
}

int timelite_file_write(struct timelite_file *file, uint64_t offset,
                        const void *buffer, size_t length, size_t *transferred)
{
    int error = check_transfer(file, offset, buffer, length, transferred);
    const unsigned char *bytes = buffer;

    if (error != 0 || length == 0)
    {
        return error;
    }
    error = seek_file(file, offset);
    if (error != 0)
    {
        return error;
    }
    while (*transferred < length)
    {
        size_t remaining = length - *transferred;
        DWORD count = remaining > TIMELITE_IO_TRANSFER_LIMIT ?
                      TIMELITE_IO_TRANSFER_LIMIT : (DWORD)remaining;
        DWORD result = 0;

        if (!WriteFile(file->handle, bytes + *transferred, count, &result, NULL))
        {
            return windows_error(GetLastError());
        }
        if (result == 0)
        {
            return EIO;
        }
        *transferred += result;
    }
    return 0;
}

int timelite_file_size(struct timelite_file *file, uint64_t *size)
{
    LARGE_INTEGER result;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    if (size == NULL)
    {
        return EINVAL;
    }
    if (!GetFileSizeEx(file->handle, &result))
    {
        return windows_error(GetLastError());
    }
    if (result.QuadPart < 0)
    {
        return EOVERFLOW;
    }
    *size = (uint64_t)result.QuadPart;
    return 0;
}

int timelite_file_truncate(struct timelite_file *file, uint64_t size)
{
    uint64_t old_size;
    int error = check_file(file);

    if (error != 0)
    {
        return error;
    }
    if (size > INT64_MAX)
    {
        return EOVERFLOW;
    }
    error = timelite_file_size(file, &old_size);
    if (error != 0)
    {
        return error;
    }
    /* A write beyond EOF zeroes the gap; SetEndOfFile alone does not promise it. */
    if (old_size < size)
    {
        const unsigned char zero = 0;
        size_t transferred;
        return timelite_file_write(file, size - 1, &zero, 1, &transferred);
    }
    error = seek_file(file, size);
    if (error != 0)
    {
        return error;
    }
    if (!SetEndOfFile(file->handle))
    {
        return windows_error(GetLastError());
    }
    return 0;
}

int timelite_file_sync(struct timelite_file *file)
{
    int error = check_file(file);
    if (error != 0)
    {
        return error;
    }
    if (!FlushFileBuffers(file->handle))
    {
        return windows_error(GetLastError());
    }
    return 0;
}

int timelite_file_close(struct timelite_file *file)
{
    HANDLE handle;
    int error = check_file(file);
    if (error != 0)
    {
        return error;
    }
    handle = file->handle;
    file->handle = NULL;
    if (!CloseHandle(handle))
    {
        return windows_error(GetLastError());
    }
    return 0;
}

/* File flush alone supplies no supported namespace provisioning contract. */
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
