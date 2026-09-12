/* Windows-only setup for the shared integration tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <wchar.h>

static int wide_test_path(const char *path, WCHAR *wide)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                               wide, MAX_PATH) != 0;
}

static int make_test_directory(char *directory, size_t capacity)
{
    WCHAR base[MAX_PATH];
    WCHAR path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, base);
    unsigned int attempt;

    if (length == 0 || length >= MAX_PATH)
    {
        return 0;
    }
    /* Atomic creation; never use or remove a pre-existing candidate. The
     * directory inherits the current user's trusted temporary-directory ACL. */
    for (attempt = 0; attempt < 100; attempt++)
    {
        int result = swprintf(path, MAX_PATH, L"%lstimelite-%lu-%lu-%u",
                              base, GetCurrentProcessId(), GetTickCount(), attempt);
        if (result < 0 || result >= MAX_PATH)
        {
            return 0;
        }
        if (CreateDirectoryW(path, NULL))
        {
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                    directory, (int)capacity, NULL, NULL) == 0)
            {
                (void)RemoveDirectoryW(path);
                return 0;
            }
            return 1;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return 0;
        }
    }
    return 0;
}

static int remove_test_file(const char *path)
{
    WCHAR wide[MAX_PATH];
    return wide_test_path(path, wide) && DeleteFileW(wide) ? 0 : -1;
}

static int remove_test_directory(const char *path)
{
    WCHAR wide[MAX_PATH];
    return wide_test_path(path, wide) && RemoveDirectoryW(wide) ? 0 : -1;
}

