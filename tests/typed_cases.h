/* Shared native/model behavior checks. The caller supplies an empty open pair. */
#ifndef TIMELITE_TYPED_CASES_H
#define TIMELITE_TYPED_CASES_H

static uint64_t typed_bits(const struct timelite_value *value)
{
    uint64_t bits;
    if (value->type == TIMELITE_VALUE_INT64)
    {
        return (uint64_t)value->data.integer;
    }
    memcpy(&bits, &value->data.floating, sizeof(bits));
    return bits;
}

static struct timelite_value typed_float(uint64_t bits)
{
    struct timelite_value value;
    value.type = TIMELITE_VALUE_FLOAT64;
    memcpy(&value.data.floating, &bits, sizeof(bits));
    return value;
}

static void typed_equal(const struct timelite_value *a, const struct timelite_value *b)
{
    assert(a->type == b->type && typed_bits(a) == typed_bits(b));
}

static void typed_input(struct timelite_typed_record *records, uint64_t time)
{
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x8000000000000001), UINT64_C(0x7fefffffffffffff),
        UINT64_C(0xffefffffffffffff), UINT64_C(0xbff4000000000000),
        UINT64_C(0x3fd5555555555555), UINT64_C(0x0010000000000000),
        UINT64_C(0x000fffffffffffff)};
    size_t i;
    for (i = 0; i < 64; i++)
    {
        records[i].series = (uint32_t)i;
        records[i].timestamp_us = time;
        if (i < 10)
        {
            records[i].value = typed_float(bits[i]);
        }
        else
        {
            records[i].value.type = TIMELITE_VALUE_INT64;
            records[i].value.data.integer = i == 10 ? INT64_MIN : i == 11 ? INT64_MAX : (int64_t)i;
        }
    }
}

static void typed_read_check(struct timelite_batches *db, uint64_t time,
                              void *work, size_t work_size)
{
    struct timelite_typed_record expected[64], records[64];
    size_t count, i;
    uint64_t sequence;
    typed_input(expected, time);
    assert(timelite_batches_next_typed(db, records, 64, &count, &sequence, work, work_size) == 0);
    assert(count == 64);
    for (i = 0; i < count; i++)
    {
        assert(records[i].series == expected[i].series && records[i].timestamp_us == time);
        typed_equal(&records[i].value, &expected[i].value);
    }
}

static void typed_cases(struct timelite_batches *db, void *work, size_t work_size)
{
    struct timelite_typed_record records[64], output_records[64], saved[64];
    struct timelite_record integer_output[64], integer_saved[64];
    struct timelite_batches before;
    struct timelite_typed_aggregate aggregate, saved_aggregate;
    struct timelite_aggregate old_aggregate = {123, 456, 789};
    struct timelite_range range = {0, 100, 0, 0};
    uint64_t sequence = 777;
    size_t count = 777, i;
    const uint64_t invalid[] = {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000001234), UINT64_C(0x7ff0000000000001)};
    TEST_CASE("typed finite values and output atomicity", 0);
    assert(timelite_float64_supported());
    typed_input(records, 20);
    before = *db;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        records[0].value = typed_float(invalid[i]);
        assert(timelite_batches_append_typed(db, records, 64, work, work_size, &sequence) == EINVAL);
        assert(sequence == 777 && memcmp(db, &before, sizeof(before)) == 0);
    }
    records[0].value.type = (enum timelite_value_type)99;
    assert(timelite_batches_append_typed(db, records, 64, work, work_size, &sequence) == EINVAL);
    typed_input(records, 20);
    assert(timelite_batches_append_typed(db, records, 64, work, 1344, &sequence) == TIMELITE_BUFFER_TOO_SMALL);
    assert(timelite_batches_append_typed(db, records, 64, work, work_size, &sequence) == 0);
    memset(output_records, 0x5a, sizeof(output_records));
    memcpy(saved, output_records, sizeof(saved));
    memset(integer_output, 0x5a, sizeof(integer_output));
    memcpy(integer_saved, integer_output, sizeof(integer_saved));
    before = *db;
    sequence = 777;
    assert(timelite_batches_next_typed(db, output_records, 63, &count, &sequence, work, work_size) == TIMELITE_BUFFER_TOO_SMALL);
    assert(count == 777 && sequence == 777 && memcmp(saved, output_records, sizeof(saved)) == 0);
    assert(timelite_batches_next(db, integer_output, 64, &count, &sequence, work, work_size) == TIMELITE_VALUE_TYPE_MISMATCH);
    assert(memcmp(integer_saved, integer_output, sizeof(integer_saved)) == 0);
    assert(memcmp(db, &before, sizeof(before)) == 0 && count == 777 && sequence == 777);
    assert(timelite_batches_aggregate_range(db, &range, &old_aggregate, work, work_size) == TIMELITE_VALUE_TYPE_MISMATCH);
    assert(old_aggregate.record_count == 123 && old_aggregate.minimum_value == 456 && old_aggregate.maximum_value == 789);
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
    assert(aggregate.record_count == 64);
    typed_equal(&aggregate.minimum_value, &records[5].value);
    typed_equal(&aggregate.maximum_value, &records[4].value);
    assert(memcmp(db, &before, sizeof(before)) == 0);
    saved_aggregate = aggregate;
    range.filter_series = 2;
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == EINVAL);
    typed_equal(&aggregate.minimum_value, &saved_aggregate.minimum_value);
    range.filter_series = 1;
    range.series = 11;
    assert(timelite_batches_next_range(db, &range, integer_output, 1, &count, &sequence, work, work_size) == 0);
    assert(count == 1 && integer_output[0].value == INT64_MAX);
    assert(timelite_batches_rewind(db) == 0);
    range.series = 1;
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
    assert(aggregate.record_count == 1);
    typed_equal(&aggregate.minimum_value, &records[1].value);
    typed_equal(&aggregate.maximum_value, &records[1].value);
    assert(timelite_batches_next_range_typed(db, &range, output_records, 1, &count, &sequence, work, work_size) == 0);
    typed_equal(&output_records[0].value, &records[1].value);
    assert(timelite_batches_rewind(db) == 0);
    typed_read_check(db, 20, work, work_size);
    assert(timelite_batches_seek(db, 20, work, work_size) == 0);
    typed_read_check(db, 20, work, work_size);
    range.from_us = range.until_us;
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
    assert(aggregate.record_count == 0 && aggregate.minimum_value.type == TIMELITE_VALUE_INT64 && aggregate.minimum_value.data.integer == 0);
}

static void typed_precision(struct timelite_batches *db, void *work, size_t work_size)
{
    /* Each pair has its own series. Expected comparison: integer versus float. */
    const int64_t integers[] = {INT64_MAX, INT64_MIN, INT64_C(9007199254740993),
        -INT64_C(9007199254740993), 0, 0, 0, 0, 1, -1, 3, -3, 0, 0};
    const uint64_t floats[] = {UINT64_C(0x43e0000000000000), UINT64_C(0xc3e0000000000000),
        UINT64_C(0x4340000000000000), UINT64_C(0xc340000000000000),
        UINT64_C(0x8000000000000000), 0, 1, UINT64_C(0x8000000000000001),
        UINT64_C(0x3ff8000000000000), UINT64_C(0xbff8000000000000),
        UINT64_C(0x4000000000000000), UINT64_C(0xc000000000000000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)};
    const int order[] = {-1, 0, 1, -1, 0, 0, -1, 1, -1, 1, 1, -1, -1, 1};
    struct timelite_typed_record records[2];
    struct timelite_typed_aggregate aggregate;
    struct timelite_range range = {30, 31, 0, 1};
    uint64_t sequence;
    size_t i;
    int reverse;
    for (reverse = 0; reverse <= 1; reverse++)
    {
        for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        {
            TEST_CASE("exact mixed aggregation", reverse * 100 + i);
            range.series = (uint32_t)(100 + reverse * 100 + i);
            records[0].series = records[1].series = range.series;
            records[0].timestamp_us = records[1].timestamp_us = 30;
            records[reverse].value.type = TIMELITE_VALUE_INT64;
            records[reverse].value.data.integer = integers[i];
            records[1 - reverse].value = typed_float(floats[i]);
            assert(timelite_batches_append_typed(db, records, 2, work, work_size, &sequence) == 0);
            assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
            assert(aggregate.record_count == 2);
            typed_equal(&aggregate.minimum_value, &records[order[i] == 0 ? 0 : order[i] < 0 ? reverse : 1 - reverse].value);
            typed_equal(&aggregate.maximum_value, &records[order[i] == 0 ? 0 : order[i] > 0 ? reverse : 1 - reverse].value);
        }
    }
    TEST_CASE("float-only aggregation and signed-zero ties", 0);
    range.series = 400;
    records[0].series = records[1].series = 400;
    records[0].value = typed_float(UINT64_C(0x8000000000000000));
    records[1].value = typed_float(0);
    assert(timelite_batches_append_typed(db, records, 2, work, work_size, &sequence) == 0);
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
    assert(aggregate.record_count == 2);
    typed_equal(&aggregate.minimum_value, &records[0].value);
    typed_equal(&aggregate.maximum_value, &records[0].value);
    records[0].value = typed_float(UINT64_C(0xbfc0000000000000));
    records[1].value = typed_float(UINT64_C(0x3fe0000000000000));
    assert(timelite_batches_append_typed(db, records, 2, work, work_size, &sequence) == 0);
    assert(timelite_batches_aggregate_range_typed(db, &range, &aggregate, work, work_size) == 0);
    assert(aggregate.record_count == 4);
    typed_equal(&aggregate.minimum_value, &records[0].value);
    typed_equal(&aggregate.maximum_value, &records[1].value);
}
#endif
