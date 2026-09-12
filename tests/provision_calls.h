/* Substitutions affect only feature 004 native operations. */
int provision_open(const char *path, int flags, ...);
int provision_fstat(int fd, struct stat *info);
int provision_fstatat(int fd, const char *path, struct stat *info, int flags);
int provision_fstatfs(int fd, struct statfs *info);
int provision_fsync(int fd);
int provision_close(int fd);
int provision_sync(struct timelite_file *file);
#if defined(__APPLE__)
int provision_entropy(void *buffer, size_t size);
#define getentropy provision_entropy
#else
ssize_t provision_random(void *buffer, size_t size, unsigned int flags);
#define getrandom provision_random
#endif
#define open provision_open
#define fstat provision_fstat
#define fstatat provision_fstatat
#define fstatfs provision_fstatfs
#define fsync provision_fsync
#define close provision_close
#define timelite_file_sync provision_sync
