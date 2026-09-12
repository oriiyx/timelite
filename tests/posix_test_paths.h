#ifndef TIMELITE_POSIX_TEST_PATHS_H
#define TIMELITE_POSIX_TEST_PATHS_H
#include <stdio.h>
#include <stdlib.h>
/* The runner supplies a private, retained directory. Direct runs use /tmp. */
static int make_test_directory(char *directory, size_t capacity)
{
    const char *root = getenv("TIMELITE_TEST_ROOT");
    int length = snprintf(directory, capacity, "%s/timelite-XXXXXX",
                          root == NULL ? "/tmp" : root);
    return length > 0 && (size_t)length < capacity && mkdtemp(directory) != NULL;
}
#endif
