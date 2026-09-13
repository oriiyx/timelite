#include "timelite.h"
#include <inttypes.h>
#include <stdio.h>

static int reopen_pair(struct timelite_batches *db, const char *database_path,
                       const char *wal_path, void *scratch, size_t scratch_size)
{
    int error = timelite_batches_close(db);
    if (error != 0)
    {
        fprintf(stderr, "close during recovery failed: %d\n", error);
        return error;
    }
    return timelite_batches_open(db, database_path, wal_path,
                                 TIMELITE_OPEN_EXISTING, scratch, scratch_size);
}

static int export_before_delete(struct timelite_batches *db, uint64_t cutoff_us,
                                void *scratch, size_t scratch_size)
{
    struct timelite_range range = {0, cutoff_us, 0, 0};
    struct timelite_record records[TIMELITE_MAX_RECORDS];
    uint64_t sequence;
    size_t count, i;
    int error = timelite_batches_seek(db, 0, scratch, scratch_size);
    if (error == TIMELITE_END)
    {
        return 0;
    }
    if (error != 0)
    {
        return error;
    }
    while ((error = timelite_batches_next_range(db, &range, records,
             TIMELITE_MAX_RECORDS, &count, &sequence, scratch, scratch_size)) == 0)
    {
        for (i = 0; i < count; i++)
        {
            /* Replace this print with an acknowledged, durable export. Do not
             * expire until the application has confirmed that export. */
            printf("export batch=%" PRIu64 " us=%" PRIu64 " value=%" PRId64 "\n",
                   sequence, records[i].timestamp_us, records[i].value);
        }
    }
    return error == TIMELITE_END ? 0 : error;
}

static int checkpoint_with_policy(struct timelite_batches *db, uint64_t cutoff_us,
                                  void *scratch, size_t scratch_size)
{
    int error = timelite_batches_checkpoint(db, scratch, scratch_size);
    if (error != TIMELITE_DATABASE_FULL)
    {
        return error;
    }
    error = export_before_delete(db, cutoff_us, scratch, scratch_size);
    if (error == 0)
    {
        error = timelite_batches_expire_before(db, cutoff_us,
                                               scratch, scratch_size);
    }
    if (error == TIMELITE_DATABASE_FULL)
    {
        /* Retention also needs temporary copy room. The application must stop
         * appending or choose a cutoff that expires more whole segments. */
        return error;
    }
    return error == 0 ? timelite_batches_checkpoint(db, scratch, scratch_size) : error;
}

static int append_with_policy(struct timelite_batches *db,
                              const struct timelite_record *records, size_t count,
                              uint64_t cutoff_us, void *scratch, size_t scratch_size,
                              uint64_t *sequence)
{
    int error = timelite_batches_append(db, records, count, scratch,
                                        scratch_size, sequence);
    if (error == TIMELITE_WAL_FULL)
    {
        error = checkpoint_with_policy(db, cutoff_us, scratch, scratch_size);
        if (error == 0)
        {
            error = timelite_batches_append(db, records, count, scratch,
                                            scratch_size, sequence);
        }
    }
    return error;
}

int main(int argc, char **argv)
{
    struct timelite_batches db;
    struct timelite_batches_status status;
    struct timelite_record batches[][2] = {{{7, 100, 10}, {8, 100, 20}},
                                            {{7, 200, 30}, {0, 0, 0}},
                                            {{7, 300, 40}, {0, 0, 0}}};
    const size_t counts[] = {2, 1, 1};
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
    for (i = 0; error == 0 && i < 2; i++)
    {
        error = append_with_policy(&db, batches[i], counts[i], 150,
                                   scratch, sizeof(scratch), &sequence);
        if (error == 0)
        {
            error = checkpoint_with_policy(&db, 150, scratch, sizeof(scratch));
        }
    }
    if (error == 0)
    {
        error = append_with_policy(&db, batches[2], counts[2], 150,
                                   scratch, sizeof(scratch), &sequence);
    }
    if (error == 0)
    {
        error = timelite_batches_get_status(&db, &status);
    }
    if (error == 0)
    {
        printf("before committed=%" PRIu64 " installed=%" PRIu64
               " pending=%" PRIu64 " floor=%" PRIu64 "\n",
               status.committed_batches, status.installed_batches,
               status.pending_batches, status.last_timestamp_us);
        error = export_before_delete(&db, 150, scratch, sizeof(scratch));
    }
    if (error == 0)
    {
        error = timelite_batches_expire_before(&db, 150, scratch, sizeof(scratch));
    }
    if (error == 0)
    {
        error = timelite_batches_get_status(&db, &status);
    }
    if (error == 0)
    {
        printf("after committed=%" PRIu64 " installed=%" PRIu64
               " pending=%" PRIu64 " floor=%" PRIu64 "\n",
               status.committed_batches, status.installed_batches,
               status.pending_batches, status.last_timestamp_us);
        error = reopen_pair(&db, argv[1], argv[2], scratch, sizeof(scratch));
    }
    if (error == 0)
    {
        error = timelite_batches_rewind(&db);
    }
    while (error == 0 && (error = timelite_batches_next(&db, output,
           TIMELITE_MAX_RECORDS, &count, &sequence, scratch, sizeof(scratch))) == 0)
    {
        printf("live batch=%" PRIu64 " us=%" PRIu64 "\n",
               sequence, output[0].timestamp_us);
    }
    if (error == TIMELITE_RECOVERY_REQUIRED)
    {
        /* Reopen, enumerate committed sequences and reconcile the uncertain
         * operation before retrying it. Never retry an append blindly. */
        int recovery_error = reopen_pair(&db, argv[1], argv[2], scratch, sizeof(scratch));
        if (recovery_error == 0)
        {
            fprintf(stderr, "recovered; inspect committed batches before retry\n");
            error = TIMELITE_RECOVERY_REQUIRED;
        }
        else
        {
            error = recovery_error;
        }
    }
    close_error = timelite_batches_close(&db);
    if (error != TIMELITE_END || close_error != 0)
    {
        fprintf(stderr, "continuous operation failed: %d/%d\n", error, close_error);
        return 1;
    }
    return 0;
}
