#ifndef TIMELITE_TEST_ASSERT_H
#define TIMELITE_TEST_ASSERT_H
#include <stdio.h>
#include <stdlib.h>
/* Always active, including direct builds with NDEBUG. */
static const char *test_case = "initialization";
static unsigned long test_boundary;
#define TEST_CASE(name, boundary) do { test_case = (name); \
    test_boundary = (unsigned long)(boundary); } while (0)
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d %s: case=%s boundary=%lu: %s\n", \
            __FILE__, __LINE__, __func__, test_case, test_boundary, #condition); \
    exit(1); } } while (0)
#endif
