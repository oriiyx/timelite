/* Serialized, bounded test policy. stdout is exclusively the wire protocol. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "timelite.h"

int main(int argc, char **argv)
{
    struct timelite_batches db;
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    char line[128];
    unsigned int operations = 0;
    int error;
    int close_error;
    if (argc == 1)
    {
        puts("usage: target_workload create|open DATABASE WAL (commands on stdin)");
        return 0;
    }
    if (argc != 4 || (strcmp(argv[1], "create") != 0 && strcmp(argv[1], "open") != 0))
    {
        return 2;
    }
    error = timelite_batches_init(&db);
    if (error == 0)
    {
        error = timelite_batches_open(&db, argv[2], argv[3],
            strcmp(argv[1], "create") == 0 ? TIMELITE_CREATE_NEW : TIMELITE_OPEN_EXISTING,
            scratch, sizeof(scratch));
    }
    if (error != 0)
    {
        fprintf(stderr, "open_error=%d\n", error);
        return error == ENOTSUP ? 77 : 1;
    }
    puts("ready");
    if (fflush(stdout) != 0)
    {
        error = EIO;
    }
    while (error == 0 && operations < 256 && fgets(line, sizeof(line), stdin) != NULL)
    {
        char operation;
        char extra;
        uint64_t value;
        uint64_t sequence = 0;
        if (sscanf(line, "%c %" SCNu64 " %c", &operation, &value, &extra) != 2 ||
            value == 0 || value > 1000000 || strchr(line, '-') != NULL)
        {
            error = EINVAL;
            break;
        }
        operations++;
        if (operation == 'A')
        {
            struct timelite_record record;
            record.series = 14;
            record.timestamp_us = value;
            record.value = (int64_t)value;
            error = timelite_batches_append(&db, &record, 1, scratch, sizeof(scratch), &sequence);
        }
        else if (operation == 'C')
        {
            error = timelite_batches_checkpoint(&db, scratch, sizeof(scratch));
        }
        else if (operation == 'R')
        {
            error = timelite_batches_expire_before(&db, value, scratch, sizeof(scratch));
        }
        else
        {
            error = EINVAL;
        }
        if (error == 0)
        {
            printf("ok %c %" PRIu64 " %" PRIu64 "\n", operation, value, sequence);
            if (fflush(stdout) != 0)
            {
                error = EIO;
            }
        }
    }
    if (ferror(stdin))
    {
        error = EIO;
    }
    close_error = timelite_batches_close(&db);
    if (error != 0 || close_error != 0)
    {
        fprintf(stderr, "operation_error=%d close_error=%d\n", error, close_error);
        return 1;
    }
    return 0;
}
