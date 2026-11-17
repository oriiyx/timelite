#include "timelite.h"
#include <inttypes.h>
#include <stdio.h>

/* Caller owns the units, buffers, checkpoint policy and both file paths. */
int main(int argc, char **argv)
{
    struct timelite_batches db;
    struct timelite_typed_record records[2] = {
        {7, 100, {TIMELITE_VALUE_INT64, {.integer = INT64_C(9007199254740993)}}},
        {8, 100, {TIMELITE_VALUE_FLOAT64, {.floating = -0.125}}}
    };
    struct timelite_range range = {0, 101, 0, 0};
    struct timelite_typed_aggregate aggregate;
    unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    uint64_t sequence;
    int error, close_error;
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s DATABASE WAL\n", argv[0]);
        return 1;
    }
    if (!timelite_float64_supported())
    {
        fputs("binary64 representation unsupported\n", stderr);
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
    error = timelite_batches_append_typed(&db, records, 2, scratch, sizeof(scratch), &sequence);
    if (error == 0)
    {
        error = timelite_batches_checkpoint(&db, scratch, sizeof(scratch));
    }
    /* Combine series only when their units are comparable. */
    if (error == 0)
    {
        error = timelite_batches_aggregate_range_typed(&db, &range, &aggregate, scratch, sizeof(scratch));
    }
    if (error == 0)
    {
        printf("count=%" PRIu64 " min=float64:%.17g max=int64:%" PRId64 "\n",
               aggregate.record_count, aggregate.minimum_value.data.floating,
               aggregate.maximum_value.data.integer);
    }
    close_error = timelite_batches_close(&db);
    if (error != 0 || close_error != 0)
    {
        fprintf(stderr, "operation/close failed: %d/%d; reopen and reconcile before retrying\n", error, close_error);
        return 1;
    }
    return 0;
}
