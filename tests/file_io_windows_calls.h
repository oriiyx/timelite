/* Compile-time substitutions, only in the Windows fault-test executable. */
#define TIMELITE_IO_TRANSFER_LIMIT 3
HANDLE test_create_file(LPCWSTR path, DWORD access, DWORD sharing,
                        LPSECURITY_ATTRIBUTES security, DWORD mode,
                        DWORD flags, HANDLE template_file);
DWORD test_file_type(HANDLE handle);
BOOL test_file_info(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info);
BOOL test_seek(HANDLE handle, LARGE_INTEGER offset, PLARGE_INTEGER position,
                DWORD method);
BOOL test_read(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD transferred,
                LPOVERLAPPED overlapped);
BOOL test_write(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD transferred,
                 LPOVERLAPPED overlapped);
BOOL test_size(HANDLE handle, PLARGE_INTEGER size);
BOOL test_truncate(HANDLE handle);
BOOL test_sync(HANDLE handle);
BOOL test_close(HANDLE handle);
#define CreateFileW test_create_file
#define GetFileType test_file_type
#define GetFileInformationByHandle test_file_info
#define SetFilePointerEx test_seek
#define ReadFile test_read
#define WriteFile test_write
#define GetFileSizeEx test_size
#define SetEndOfFile test_truncate
#define FlushFileBuffers test_sync
#define CloseHandle test_close
