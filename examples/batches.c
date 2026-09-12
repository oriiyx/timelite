#include "timelite.h"
#include <stdio.h>
#include <inttypes.h>

/* Supply two unused paths in a stable, already-durable local directory. */
int main(int argc, char **argv)
{
    struct timelite_batches db;
    struct timelite_record input[] = {{7, UINT64_C(1700000000000000), 23500},
                                      {7, UINT64_C(1700000060000000), 23625}};
    struct timelite_record output[TIMELITE_MAX_RECORDS];
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    uint64_t sequence;
    size_t count, i;
    int error, close_error;
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s DATABASE WAL\n", argv[0]);
        return 1;
    }
    (void)timelite_batches_init(&db);
    error = timelite_batches_open(&db, argv[1], argv[2], TIMELITE_CREATE_NEW,
                                  scratch, sizeof(scratch));
    if (error != 0)
    {
        fprintf(stderr, "create failed: %d (partial files may remain)\n", error);
        return 1;
    }
    /* Application convention: series 7 measures thousandths of a degree C. */
    error = timelite_batches_append(&db, input, 1, scratch, sizeof(scratch), &sequence);
    if (error == 0)
    {
        /* Install committed batches into the main file and reclaim the WAL. */
        error = timelite_batches_checkpoint(&db, scratch, sizeof(scratch));
    }
    if (error == 0)
    {
        error = timelite_batches_append(&db, input + 1, 1, scratch, sizeof(scratch), &sequence);
    }
    close_error = timelite_batches_close(&db);
    if (error != 0)
    {
        fprintf(stderr, "append/checkpoint failed: %d; reopen and inspect before retrying\n", error);
        return 1;
    }
    if (close_error != 0)
    {
        fprintf(stderr, "close failed: %d; handle consumed, do not retry close\n", close_error);
        return 1;
    }
    error = timelite_batches_open(&db, argv[1], argv[2], TIMELITE_OPEN_EXISTING,
                                  scratch, sizeof(scratch));
    if (error != 0)
    {
        fprintf(stderr, "reopen failed: %d\n", error);
        return 1;
    }
    while ((error = timelite_batches_next(&db, output, TIMELITE_MAX_RECORDS,
                                          &count, &sequence, scratch,
                                          sizeof(scratch))) == 0)
    {
        for (i = 0; i < count; i++)
        {
            printf("batch=%" PRIu64 " series=%" PRIu32 " us=%" PRIu64
                   " value=%" PRId64 "\n", sequence, output[i].series,
                   output[i].timestamp_us, output[i].value);
        }
    }
    close_error = timelite_batches_close(&db);
    if (error != TIMELITE_END || close_error != 0)
    {
        fprintf(stderr, "read/close failed: %d/%d\n", error, close_error);
        return 1;
    }
    return 0;
}
