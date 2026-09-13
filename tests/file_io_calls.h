/* Compile-time substitution, only in the fault-test executable. */
int test_open(const char *path, int flags, ...);
int test_fstat(int fd, struct stat *info);
ssize_t test_pread(int fd, void *buffer, size_t count, off_t offset);
ssize_t test_pwrite(int fd, const void *buffer, size_t count, off_t offset);
int test_ftruncate(int fd, off_t size);
int test_sync(int fd, ...);
int test_close(int fd);
int test_flock(int fd, int operation);
#define flock test_flock
#define open test_open
#define fstat test_fstat
#define pread test_pread
#define pwrite test_pwrite
#define ftruncate test_ftruncate
#define fdatasync test_sync
#define fcntl test_sync
#define close test_close
