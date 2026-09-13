/* timelite-inspect: offline inspection of a Timelite database/WAL pair.
 *
 * verify DATABASE WAL  read-only. Parses the documented byte layouts with
 *                      stdio only, reports what is on disk and predicts what
 *                      the library's recovery would do. Never writes.
 * status DATABASE WAL  opens the pair through the public API, which performs
 *                      normal recovery, and prints timelite_batches_status.
 *
 * Exit codes: 0 coherent, 1 inconsistency found, 2 usage or I/O error.
 * Output is one fact per line as key=value. Fixed buffers only.
 *
 * Format constants below are copies. The source of truth is timelite.c; the
 * inventory test `inspect` fails when the two diverge. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "timelite.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Feature 004: 32-byte pair headers, 32-byte frame header and commit,
 * 20-byte records, at most 64 records per frame. */
#define INSPECT_HEADER 32u
#define INSPECT_FRAME_MIN 84u
#define INSPECT_RECORD 20u
/* Feature 006: manifest slots at 32 and 96, segments from 160, minimum
 * segment 32 + 84 bytes. */
#define INSPECT_DATA_START UINT64_C(160)
#define INSPECT_SEGMENT_MIN UINT64_C(116)
/* Feature 007: format marker 7 in an install manifest with a timestamp.
 * Feature 010: format marker 10 in a retention manifest, phases 0..3. */
#define INSPECT_MARKER_TIME 7u
#define INSPECT_MARKER_RETENTION 10u

#define INSPECT_MAX_PROBLEMS 32u

struct inspect_source
{
    FILE *file;
    uint64_t size;
};

/* One manifest slot as feature 006 (TLINSTAL), 007 (marker 7) or 010
 * (TLRETAIN) define it. well_formed: magic and checksum pass; valid: every
 * range rule passes too, so the library would accept it. */
struct inspect_slot
{
    int present, well_formed, valid, retention, has_time;
    uint64_t revision, data_end, last, last_time, tail, root, count;
    unsigned int phase, marker;
    const char *kind;
    const char *reason;
};

/* Segment header as feature 006 (32 bytes) or 007 (64 bytes) define it. */
struct inspect_segment
{
    uint64_t first, last, minimum, maximum, previous, skip;
    uint32_t body;
    unsigned int header_size;
    int ordered;
    const char *kind;
    const char *reason;
};

/* One frame as feature 004 defines it, with its record time summary. */
struct inspect_frame
{
    size_t length, records;
    uint64_t first_time, last_time, minimum, maximum;
    int ordered;
    const char *reason;
};

/* Everything verify learns, in the order the library's open learns it. */
struct inspect_state
{
    FILE *out;
    struct inspect_source database, wal;
    unsigned int problems;
    /* First error timelite_batches_open would return, or 0. */
    int predicted_error;
    const char *predicted_stage;
    int io_error;
    /* Manifest. */
    struct inspect_slot slots[2];
    struct inspect_slot *active;
    int legacy, retention, has_time;
    unsigned int phase;
    uint64_t generation, data_end, installed, installed_time, root, tail;
    /* Segments and frames. */
    uint64_t live, records, first_live, unreadable, time_min, time_max;
    uint64_t floor_time;
    int have_times, floor_known;
    const char *floor_source;
    /* WAL. */
    int stale, wal_ordered;
    uint64_t sequence, wal_end, wal_frames, wal_first_sequence, wal_first_time, wal_last_time;
};

static unsigned char inspect_bytes[TIMELITE_BATCH_SCRATCH];

static uint32_t inspect_decode32(const unsigned char *p)
{
    uint32_t value = 0;
    unsigned int i;
    for (i = 0; i < 4; i++)
    {
        value |= (uint32_t)p[i] << (8 * i);
    }
    return value;
}

static uint64_t inspect_decode64(const unsigned char *p)
{
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; i++)
    {
        value |= (uint64_t)p[i] << (8 * i);
    }
    return value;
}

/* CRC-32/ISO-HDLC as in feature 004: reflected 0xedb88320, init/final ~0. */
static uint32_t inspect_checksum(const unsigned char *p, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;
    for (i = 0; i < length; i++)
    {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}

static uint64_t inspect_low_bit(uint64_t n)
{
    return n & (~n + 1);
}

static const char *inspect_error_name(int error)
{
    switch (error)
    {
        case 0: return "ok";
        case TIMELITE_INVALID_DATABASE: return "TIMELITE_INVALID_DATABASE";
        case TIMELITE_UNSUPPORTED_VERSION: return "TIMELITE_UNSUPPORTED_VERSION";
        case TIMELITE_END: return "TIMELITE_END";
        case TIMELITE_BUFFER_TOO_SMALL: return "TIMELITE_BUFFER_TOO_SMALL";
        case TIMELITE_RECOVERY_REQUIRED: return "TIMELITE_RECOVERY_REQUIRED";
        case TIMELITE_WAL_FULL: return "TIMELITE_WAL_FULL";
        case TIMELITE_PAIR_MISMATCH: return "TIMELITE_PAIR_MISMATCH";
        case TIMELITE_DATABASE_FULL: return "TIMELITE_DATABASE_FULL";
        case TIMELITE_OUT_OF_ORDER: return "TIMELITE_OUT_OF_ORDER";
        case EINVAL: return "EINVAL";
        case ENOENT: return "ENOENT";
        case ENOTSUP: return "ENOTSUP";
        case EBADF: return "EBADF";
        case EACCES: return "EACCES";
        case EEXIST: return "EEXIST";
        case EIO: return "EIO";
        case ENOSPC: return "ENOSPC";
        case EOVERFLOW: return "EOVERFLOW";
        default: return "errno";
    }
}

static const char *inspect_phase_name(unsigned int phase)
{
    static const char *const names[] = {"NORMAL", "PREPARE", "TAIL", "CLEANUP"};
    return phase <= 3 ? names[phase] : "unknown";
}

static void inspect_problem(struct inspect_state *state, uint64_t offset,
                            const char *file, const char *detail)
{
    state->problems++;
    if (state->problems <= INSPECT_MAX_PROBLEMS)
    {
        fprintf(state->out, "inconsistency.%u.file=%s\n", state->problems, file);
        fprintf(state->out, "inconsistency.%u.offset=%" PRIu64 "\n", state->problems, offset);
        fprintf(state->out, "inconsistency.%u.detail=%s\n", state->problems, detail);
    }
}

/* The library returns its first error; keep only the first prediction. */
static void inspect_predict(struct inspect_state *state, int error, const char *stage)
{
    if (state->predicted_error == 0)
    {
        state->predicted_error = error;
        state->predicted_stage = stage;
    }
}

/* Physical size by streaming, so it does not depend on the range of long. */
static int inspect_open(struct inspect_source *source, const char *path,
                        const char *label, FILE *out)
{
    static unsigned char chunk[4096];
    size_t got;
    source->size = 0;
    source->file = fopen(path, "rb");
    if (source->file == NULL)
    {
        fprintf(out, "error=cannot open %s: %s\n", label, strerror(errno));
        return 2;
    }
    while ((got = fread(chunk, 1, sizeof(chunk), source->file)) != 0)
    {
        source->size += got;
    }
    if (ferror(source->file))
    {
        fprintf(out, "error=cannot read %s: %s\n", label, strerror(errno));
        return 2;
    }
    return 0;
}

/* Returns bytes available at offset (short at EOF), or -1 on an I/O error.
 * Valid layouts never place data beyond the 1 GiB capacity, so an offset
 * that does not fit in long is treated as end of file. */
static long inspect_read(struct inspect_source *source, uint64_t offset,
                         unsigned char *buffer, size_t length)
{
    size_t got;
    if (offset >= source->size || offset > (uint64_t)LONG_MAX)
    {
        return 0;
    }
    if (fseek(source->file, (long)offset, SEEK_SET) != 0)
    {
        return -1;
    }
    got = fread(buffer, 1, length, source->file);
    if (got != length && ferror(source->file))
    {
        return -1;
    }
    return (long)got;
}

/* --- parsers for one structure each ------------------------------------- */

/* Feature 004 pair header. Returns the library's open result for it. */
static int inspect_pair_header(struct inspect_state *state, struct inspect_source *source,
                               const char *label, const char *magic, unsigned char *header)
{
    FILE *out = state->out;
    long got = inspect_read(source, 0, header, INSPECT_HEADER);
    uint32_t version;
    size_t i;
    if (got < 0)
    {
        fprintf(out, "error=cannot read %s header\n", label);
        state->io_error = 1;
        return EIO;
    }
    if (got < 12 || memcmp(header, magic, 8) != 0)
    {
        inspect_problem(state, 0, label, got < 12 ? "shorter than a pair header" : "wrong magic");
        return TIMELITE_INVALID_DATABASE;
    }
    version = inspect_decode32(header + 8);
    fprintf(out, "%s_magic=%.8s\n", label, (const char *)header);
    fprintf(out, "%s_version=%" PRIu32 "\n", label, version);
    if (version == 0)
    {
        inspect_problem(state, 8, label, "version 0");
        return TIMELITE_INVALID_DATABASE;
    }
    if (version != 2)
    {
        inspect_problem(state, 8, label, version == 1 ?
                        "version 1 lifecycle file; the batch API rejects it without migration" :
                        "unknown version");
        return TIMELITE_UNSUPPORTED_VERSION;
    }
    if (got < (long)INSPECT_HEADER)
    {
        inspect_problem(state, (uint64_t)got, label, "version 2 header cut short");
        return TIMELITE_INVALID_DATABASE;
    }
    if (inspect_checksum(header, 28) != inspect_decode32(header + 28))
    {
        inspect_problem(state, 28, label, "pair header checksum mismatch");
        return TIMELITE_INVALID_DATABASE;
    }
    fprintf(out, "%s_identity=", label);
    for (i = 0; i < 16; i++)
    {
        fprintf(out, "%02x", header[12 + i]);
    }
    fputc('\n', out);
    return 0;
}

static void inspect_slot_parse(const unsigned char *m, uint64_t size, struct inspect_slot *slot)
{
    static const unsigned char zero[64];
    memset(slot, 0, sizeof(*slot));
    slot->present = memcmp(m, zero, 64) != 0;
    slot->reason = "";
    slot->kind = "unknown";
    if (!slot->present)
    {
        slot->reason = "empty";
        return;
    }
    slot->revision = inspect_decode64(m + 8);
    slot->data_end = inspect_decode64(m + 16);
    slot->last = inspect_decode64(m + 24);
    slot->last_time = inspect_decode64(m + 32);
    slot->marker = inspect_decode32(m + 48);
    if (memcmp(m, "TLRETAIN", 8) == 0)
    {
        slot->kind = "TLRETAIN";
        slot->retention = 1;
        slot->has_time = 1;
        slot->tail = inspect_decode32(m + 40);
        slot->root = inspect_decode32(m + 44);
        slot->count = inspect_decode32(m + 52);
        slot->phase = inspect_decode32(m + 56);
        if (inspect_checksum(m, 60) != inspect_decode32(m + 60))
        {
            slot->reason = "checksum mismatch";
            return;
        }
        if (slot->marker != INSPECT_MARKER_RETENTION)
        {
            slot->reason = "format marker is not 10";
            return;
        }
        slot->well_formed = 1;
        if (slot->revision == 0 || slot->phase > 3)
        {
            slot->reason = "revision 0 or phase above 3";
        }
        else if (slot->root < INSPECT_DATA_START || slot->root > slot->data_end ||
                 slot->data_end > size || slot->data_end > TIMELITE_DATABASE_CAPACITY)
        {
            slot->reason = "root or data_end outside the file or capacity";
        }
        else if (slot->count > (slot->data_end - slot->root) / INSPECT_SEGMENT_MIN ||
                 slot->count > slot->last)
        {
            slot->reason = "segment count exceeds the extent or the sequence high-water";
        }
        else if (slot->count != 0 ? !(slot->tail >= slot->root && slot->tail < slot->data_end) :
                 !(slot->tail == 0 && slot->root == slot->data_end))
        {
            slot->reason = "last segment offset disagrees with the count";
        }
        else if (slot->phase == 2 ? slot->data_end - slot->root > slot->root - INSPECT_DATA_START :
                 slot->root != INSPECT_DATA_START)
        {
            slot->reason = "root disagrees with the phase";
        }
        else
        {
            slot->valid = 1;
        }
        return;
    }
    if (memcmp(m, "TLINSTAL", 8) != 0)
    {
        slot->reason = "unknown magic";
        return;
    }
    slot->kind = "TLINSTAL";
    slot->count = slot->revision;
    slot->tail = inspect_decode64(m + 40);
    slot->root = INSPECT_DATA_START;
    slot->has_time = slot->marker == INSPECT_MARKER_TIME;
    if (memcmp(m + 32, zero, 28) != 0 &&
        (slot->marker != INSPECT_MARKER_TIME || memcmp(m + 52, zero, 8) != 0 || slot->revision == 0))
    {
        slot->reason = "reserved bytes are not zero";
        return;
    }
    if (inspect_checksum(m, 60) != inspect_decode32(m + 60))
    {
        slot->reason = "checksum mismatch";
        return;
    }
    slot->well_formed = 1;
    if (slot->data_end < INSPECT_DATA_START || slot->data_end > TIMELITE_DATABASE_CAPACITY ||
        slot->data_end > size)
    {
        slot->reason = "data_end outside the file or capacity";
    }
    else if (slot->revision == 0 ? (slot->data_end != INSPECT_DATA_START || slot->last != 0) :
             (slot->revision > (TIMELITE_DATABASE_CAPACITY - INSPECT_DATA_START) / INSPECT_SEGMENT_MIN ||
              slot->last < slot->revision ||
              slot->data_end < INSPECT_DATA_START + slot->revision * INSPECT_SEGMENT_MIN))
    {
        slot->reason = "generation disagrees with data_end or the last sequence";
    }
    else
    {
        slot->valid = 1;
    }
}

static int inspect_segment_parse(struct inspect_source *source, uint64_t offset, uint64_t end,
                                 struct inspect_segment *segment)
{
    unsigned char header[64];
    uint64_t frames;
    long got;
    memset(segment, 0, sizeof(*segment));
    segment->reason = "";
    if (offset > end || end - offset < 32)
    {
        segment->reason = "no room for a segment header before data_end";
        return 0;
    }
    got = inspect_read(source, offset, header, 64);
    if (got < 32)
    {
        segment->reason = got < 0 ? "read error" : "segment header cut short";
        return 0;
    }
    if (memcmp(header, "TLSPAN07", 8) == 0)
    {
        segment->kind = "TLSPAN07";
        segment->ordered = 1;
        segment->header_size = 64;
    }
    else if (memcmp(header, "TLUNOR07", 8) == 0)
    {
        segment->kind = "TLUNOR07";
        segment->header_size = 64;
    }
    else if (memcmp(header, "TLSEGMNT", 8) == 0)
    {
        segment->kind = "TLSEGMNT";
        segment->header_size = 32;
    }
    else
    {
        segment->reason = "unknown segment magic";
        return 0;
    }
    if (segment->header_size == 64 && (end - offset < 64 || got < 64))
    {
        segment->reason = "64-byte segment header cut short";
        return 0;
    }
    if (inspect_checksum(header, segment->header_size - 4) !=
        inspect_decode32(header + segment->header_size - 4))
    {
        segment->reason = "segment header checksum mismatch";
        return 0;
    }
    segment->first = inspect_decode64(header + 8);
    segment->last = inspect_decode64(header + 16);
    segment->body = inspect_decode32(header + 24);
    if (segment->first == 0 || segment->last < segment->first ||
        segment->body > TIMELITE_WAL_CAPACITY - 32 ||
        segment->body > end - offset - segment->header_size)
    {
        segment->reason = "segment sequences or body length out of range";
        return 0;
    }
    frames = segment->last - segment->first + 1;
    if (frames > segment->body / INSPECT_FRAME_MIN || segment->body > frames * TIMELITE_BATCH_SCRATCH)
    {
        segment->reason = "frame count disagrees with body length";
        return 0;
    }
    segment->minimum = 0;
    segment->maximum = UINT64_MAX;
    if (segment->header_size == 64)
    {
        segment->minimum = inspect_decode64(header + 28);
        segment->maximum = inspect_decode64(header + 36);
        segment->previous = inspect_decode64(header + 44);
        segment->skip = inspect_decode64(header + 52);
        if (segment->minimum > segment->maximum ||
            (segment->previous != 0 && (segment->previous < INSPECT_DATA_START || segment->previous >= offset)) ||
            (segment->skip != 0 && (segment->skip < INSPECT_DATA_START || segment->skip >= offset)))
        {
            segment->reason = "span or backward links out of range";
            return 0;
        }
    }
    return 1;
}

/* Returns 0 valid, TIMELITE_END for a validated header whose declared end
 * passes `end` (incomplete suffix), TIMELITE_INVALID_DATABASE for damage,
 * EIO for a read error. */
static int inspect_frame_parse(struct inspect_source *source, uint64_t offset, uint64_t end,
                               uint64_t sequence, unsigned char *bytes, struct inspect_frame *frame)
{
    uint32_t count, body_length;
    const unsigned char *commit;
    size_t total, i;
    long got;
    memset(frame, 0, sizeof(*frame));
    frame->ordered = 1;
    frame->reason = "";
    if (end - offset < 32)
    {
        frame->reason = "fewer than 32 bytes remain for a frame header";
        return TIMELITE_INVALID_DATABASE;
    }
    got = inspect_read(source, offset, bytes, 32);
    if (got < 0)
    {
        frame->reason = "read error";
        return EIO;
    }
    if (got < 32)
    {
        frame->reason = "frame header cut short";
        return TIMELITE_INVALID_DATABASE;
    }
    count = inspect_decode32(bytes + 16);
    if (memcmp(bytes, "TLBATCH!", 8) != 0 || count == 0 || count > TIMELITE_MAX_RECORDS ||
        inspect_decode32(bytes + 20) != 32 + count * INSPECT_RECORD ||
        inspect_decode32(bytes + 24) != 0 ||
        inspect_checksum(bytes, 28) != inspect_decode32(bytes + 28))
    {
        frame->reason = "frame header invalid";
        return TIMELITE_INVALID_DATABASE;
    }
    if (inspect_decode64(bytes + 8) != sequence)
    {
        frame->reason = "frame sequence is not the expected next sequence";
        return TIMELITE_INVALID_DATABASE;
    }
    body_length = inspect_decode32(bytes + 20);
    total = (size_t)body_length + 32;
    if ((uint64_t)total > end - offset)
    {
        frame->reason = "validated header, but the frame ends past the available bytes";
        return TIMELITE_END;
    }
    got = inspect_read(source, offset + 32, bytes + 32, total - 32);
    if (got < 0)
    {
        frame->reason = "read error";
        return EIO;
    }
    if ((size_t)got != total - 32)
    {
        frame->reason = "frame body cut short";
        return TIMELITE_INVALID_DATABASE;
    }
    commit = bytes + body_length;
    if (memcmp(commit, "TLCOMMIT", 8) != 0 || inspect_decode64(commit + 8) != sequence ||
        inspect_decode32(commit + 16) != count || inspect_decode32(commit + 20) != body_length ||
        inspect_decode32(commit + 28) != inspect_checksum(commit, 28))
    {
        frame->reason = "commit record invalid";
        return TIMELITE_INVALID_DATABASE;
    }
    if (inspect_decode32(commit + 24) != inspect_checksum(bytes, body_length))
    {
        frame->reason = "body checksum mismatch";
        return TIMELITE_INVALID_DATABASE;
    }
    frame->length = total;
    frame->records = count;
    for (i = 0; i < count; i++)
    {
        uint64_t time = inspect_decode64(bytes + 32 + i * INSPECT_RECORD + 4);
        if (i == 0)
        {
            frame->first_time = frame->minimum = frame->maximum = time;
        }
        else if (time < frame->last_time)
        {
            frame->ordered = 0;
        }
        if (time < frame->minimum)
        {
            frame->minimum = time;
        }
        if (time > frame->maximum)
        {
            frame->maximum = time;
        }
        frame->last_time = time;
    }
    return 0;
}

/* --- verify, one stage per function in the library's open order -------- */

static void inspect_count_frame(struct inspect_state *state, const struct inspect_frame *frame)
{
    state->records += frame->records;
    if (!state->have_times)
    {
        state->time_min = frame->minimum;
        state->time_max = frame->maximum;
        state->have_times = 1;
    }
    if (frame->minimum < state->time_min)
    {
        state->time_min = frame->minimum;
    }
    if (frame->maximum > state->time_max)
    {
        state->time_max = frame->maximum;
    }
}

static void inspect_headers(struct inspect_state *state)
{
    unsigned char header[INSPECT_HEADER], wal_header[INSPECT_HEADER];
    int error = inspect_pair_header(state, &state->database, "database", "TIMELITE", header);
    if (error != 0)
    {
        inspect_predict(state, error, "database header");
        return;
    }
    error = inspect_pair_header(state, &state->wal, "wal", "TIMEWAL!", wal_header);
    if (error != 0)
    {
        inspect_predict(state, error, "wal header");
    }
    else if (memcmp(header + 12, wal_header + 12, 16) != 0)
    {
        fprintf(state->out, "pair_identity=mismatch\n");
        inspect_problem(state, 12, "wal", "identity differs from the database identity");
        inspect_predict(state, TIMELITE_PAIR_MISMATCH, "pair identity");
    }
    else
    {
        fprintf(state->out, "pair_identity=match\n");
    }
}

/* Mirrors read_manifests: parity selects the slot, the highest revision
 * wins, no valid slot means empty only for a file of at most 160 bytes. */
static void inspect_manifests(struct inspect_state *state)
{
    FILE *out = state->out;
    unsigned char slot_bytes[64];
    unsigned int index;
    state->legacy = state->database.size == 32;
    fprintf(out, "database_layout=%s\n", state->legacy ? "legacy-004" : "manifest");
    for (index = 0; index < 2 && !state->legacy; index++)
    {
        struct inspect_slot *slot = &state->slots[index];
        long got = inspect_read(&state->database, 32 + 64 * (uint64_t)index, slot_bytes, 64);
        if (got < 0)
        {
            fprintf(out, "error=cannot read database slot %u\n", index);
            state->io_error = 1;
            return;
        }
        if (got < 64)
        {
            memset(slot, 0, sizeof(*slot));
            slot->kind = "unknown";
            slot->reason = "cut short";
            slot->present = got > 0;
        }
        else
        {
            inspect_slot_parse(slot_bytes, state->database.size, slot);
        }
        if (slot->present && slot->valid && (slot->revision & 1) != index)
        {
            slot->valid = 0;
            slot->well_formed = 0;
            slot->reason = "revision parity does not select this slot";
        }
        if (slot->valid && (state->active == NULL || slot->revision > state->active->revision))
        {
            state->active = slot;
        }
    }
    /* A well-formed older slot whose extent the newer phase truncated is
     * superseded, not damaged; anything else that is not valid is. */
    for (index = 0; index < 2 && !state->legacy; index++)
    {
        struct inspect_slot *slot = &state->slots[index];
        int superseded = slot->present && !slot->valid && slot->well_formed &&
                         state->active != NULL && slot->revision < state->active->revision;
        fprintf(out, "slot.%u.state=%s\n", index, !slot->present ? "empty" : slot->valid ? "valid" :
                superseded ? "superseded" : "invalid");
        if (slot->present)
        {
            fprintf(out, "slot.%u.kind=%s\n", index, slot->kind);
            fprintf(out, "slot.%u.revision=%" PRIu64 "\n", index, slot->revision);
        }
        if (slot->present && !slot->valid)
        {
            fprintf(out, "slot.%u.reason=%s\n", index, slot->reason);
            if (!superseded)
            {
                inspect_problem(state, 32 + 64 * (uint64_t)index, "database", slot->reason);
            }
        }
    }
    if (state->legacy)
    {
        fprintf(out, "active_slot=none\n");
        fprintf(out, "manifest=legacy-004 (32-byte database, nothing installed)\n");
    }
    else if (state->active == NULL)
    {
        fprintf(out, "active_slot=none\n");
        if (state->database.size > INSPECT_DATA_START)
        {
            inspect_problem(state, 32, "database", "no valid manifest slot in a database larger than 160 bytes");
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "manifests");
        }
        else
        {
            fprintf(out, "manifest=none (database of at most 160 bytes is treated as empty)\n");
        }
    }
    else
    {
        const struct inspect_slot *active = state->active;
        fprintf(out, "active_slot=%u\n", active == &state->slots[0] ? 0 : 1);
        fprintf(out, "manifest_kind=%s\n", active->retention ? "retention-010" :
                active->has_time ? "install-007" : "install-006");
        fprintf(out, "revision=%" PRIu64 "\n", active->revision);
        state->generation = active->count;
        state->data_end = active->data_end;
        state->installed = active->last;
        state->installed_time = active->last_time;
        state->has_time = active->has_time;
        state->retention = active->retention;
        state->root = active->root;
        state->tail = active->tail;
        state->phase = active->retention ? active->phase : 0;
        fprintf(out, "generation=%" PRIu64 "\n", state->generation);
        fprintf(out, "installed_last_sequence=%" PRIu64 "\n", state->installed);
        if (state->has_time)
        {
            fprintf(out, "installed_last_timestamp_us=%" PRIu64 "\n", state->installed_time);
        }
        fprintf(out, "root=%" PRIu64 "\n", state->root);
        fprintf(out, "last_segment_offset=%" PRIu64 "\n", state->tail);
        if (state->retention)
        {
            fprintf(out, "retention_phase=%s\n", inspect_phase_name(state->phase));
        }
    }
    fprintf(out, "database_logical_end=%" PRIu64 "\n", state->data_end);
}

/* Frames of one segment. The library validates these when read, so damage
 * here leaves open succeeding and the read failing at that batch. Returns
 * the number of frames read. */
static uint64_t inspect_segment_frames(struct inspect_state *state, uint64_t offset,
                                       const struct inspect_segment *segment)
{
    struct inspect_frame frame;
    uint64_t position = offset + segment->header_size, end = position + segment->body;
    uint64_t sequence = segment->first;
    int error = 0;
    while (position < end)
    {
        error = inspect_frame_parse(&state->database, position, end, sequence, inspect_bytes, &frame);
        if (error != 0)
        {
            inspect_problem(state, position, "database", frame.reason);
            if (state->unreadable == 0)
            {
                state->unreadable = sequence;
            }
            break;
        }
        inspect_count_frame(state, &frame);
        if (segment->ordered && (!frame.ordered || frame.first_time < segment->minimum ||
                                 frame.last_time > segment->maximum))
        {
            inspect_problem(state, position, "database", "record times leave the ordered segment span");
        }
        state->floor_time = frame.last_time;
        state->floor_known = 1;
        state->floor_source = "segment_scan";
        position += frame.length;
        sequence++;
    }
    if (error == 0 && sequence - 1 != segment->last)
    {
        inspect_problem(state, position, "database", "segment body holds a different number of frames than its header");
    }
    return sequence - segment->first;
}

/* Mirrors check_segments: exactly `generation` headers from the root, each
 * continuing the sequence and the backward links, ending at data_end. */
static void inspect_segments(struct inspect_state *state)
{
    FILE *out = state->out;
    struct inspect_segment segment;
    uint64_t offsets[24] = {0};
    uint64_t offset = state->root, previous = 0, sequence = 0, n, frames;
    unsigned int bit, j;
    int damaged_final = 0;
    memset(&segment, 0, sizeof(segment));
    for (n = 1; n <= state->generation; n++)
    {
        if (!inspect_segment_parse(&state->database, offset, state->data_end, &segment))
        {
            inspect_problem(state, offset, "database", segment.reason);
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "segment header");
            break;
        }
        fprintf(out, "segment.%" PRIu64 ".offset=%" PRIu64 "\n", n, offset);
        fprintf(out, "segment.%" PRIu64 ".kind=%s\n", n, segment.kind);
        fprintf(out, "segment.%" PRIu64 ".first_sequence=%" PRIu64 "\n", n, segment.first);
        fprintf(out, "segment.%" PRIu64 ".last_sequence=%" PRIu64 "\n", n, segment.last);
        fprintf(out, "segment.%" PRIu64 ".body_bytes=%" PRIu32 "\n", n, segment.body);
        if (segment.header_size == 64)
        {
            fprintf(out, "segment.%" PRIu64 ".minimum_us=%" PRIu64 "\n", n, segment.minimum);
            fprintf(out, "segment.%" PRIu64 ".maximum_us=%" PRIu64 "\n", n, segment.maximum);
        }
        if (sequence == UINT64_MAX || segment.first <= sequence ||
            (!state->retention && segment.first != sequence + 1))
        {
            inspect_problem(state, offset + 8, "database", "segment sequence does not continue from the previous segment");
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "segment sequence");
        }
        /* The manifest capacity checks bound bit below 24. */
        for (bit = 0; bit < 23 && (UINT64_C(1) << bit) != inspect_low_bit(n); bit++)
        {
        }
        if (segment.header_size == 64 && (segment.previous != previous || segment.skip != offsets[bit]))
        {
            inspect_problem(state, offset + 44, "database", "backward links do not match the walked chain");
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "segment links");
        }
        for (j = 0; j <= bit; j++)
        {
            offsets[j] = offset;
        }
        if (state->live == 0)
        {
            state->first_live = segment.first;
        }
        state->live += segment.last - segment.first + 1;
        frames = inspect_segment_frames(state, offset, &segment);
        fprintf(out, "segment.%" PRIu64 ".frames_read=%" PRIu64 "\n", n, frames);
        damaged_final = frames != segment.last - segment.first + 1;
        previous = offset;
        sequence = segment.last;
        offset += segment.header_size + segment.body;
    }
    if (state->predicted_error == 0 && n > state->generation)
    {
        if (offset != state->data_end)
        {
            inspect_problem(state, offset, "database", "segment chain does not end at data_end");
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "segment chain end");
        }
        if (state->generation != 0)
        {
            if (sequence > state->installed || (!state->retention && sequence != state->installed))
            {
                inspect_problem(state, 24, "database", "manifest last sequence disagrees with the final segment");
                inspect_predict(state, TIMELITE_INVALID_DATABASE, "installed sequence");
            }
            if (state->has_time && state->tail != previous)
            {
                inspect_problem(state, 40, "database", "manifest last segment offset disagrees with the chain");
                inspect_predict(state, TIMELITE_INVALID_DATABASE, "last segment offset");
            }
            if (state->has_time && segment.ordered && !state->retention &&
                state->installed_time != segment.maximum)
            {
                inspect_problem(state, 32, "database", "manifest timestamp disagrees with the final segment span");
                inspect_predict(state, TIMELITE_INVALID_DATABASE, "installed timestamp");
            }
            if (!state->has_time && damaged_final)
            {
                /* Feature 006 manifests carry no timestamp: open scans the
                 * final segment's frames and fails closed on damage. */
                inspect_predict(state, TIMELITE_INVALID_DATABASE, "final legacy segment scan");
            }
        }
    }
    if (state->has_time)
    {
        state->floor_time = state->installed_time;
        state->floor_known = 1;
        state->floor_source = "manifest";
    }
    else if (state->generation == 0)
    {
        state->floor_time = 0;
        state->floor_known = 1;
        state->floor_source = "none";
    }
    fprintf(out, "installed_batches=%" PRIu64 "\n", state->live);
}

/* Mirrors the WAL part of open: a first frame at or below the installed
 * sequence is a stale WAL from an interrupted reclaim; otherwise frames
 * continue from installed + 1 and a validated incomplete tail ends the scan. */
static void inspect_wal(struct inspect_state *state)
{
    FILE *out = state->out;
    struct inspect_frame frame;
    uint64_t offset = 32;
    int error;
    state->sequence = state->installed;
    state->wal_ordered = 1;
    if (state->wal.size < 32 || state->wal.size > TIMELITE_WAL_CAPACITY)
    {
        inspect_problem(state, state->wal.size, "wal", "WAL size below its header or above 64 MiB");
        inspect_predict(state, TIMELITE_INVALID_DATABASE, "wal size");
        return;
    }
    if (state->wal.size > 32)
    {
        long got = inspect_read(&state->wal, 32, inspect_bytes, 32);
        uint32_t count = got == 32 ? inspect_decode32(inspect_bytes + 16) : 0;
        if (got < 0)
        {
            fprintf(out, "error=cannot read wal\n");
            state->io_error = 1;
            return;
        }
        if (got < 32 || memcmp(inspect_bytes, "TLBATCH!", 8) != 0 || count == 0 ||
            count > TIMELITE_MAX_RECORDS ||
            inspect_decode32(inspect_bytes + 20) != 32 + count * INSPECT_RECORD ||
            inspect_decode32(inspect_bytes + 24) != 0 ||
            inspect_checksum(inspect_bytes, 28) != inspect_decode32(inspect_bytes + 28))
        {
            inspect_problem(state, 32, "wal", got < 32 ?
                            "1..31 bytes after the WAL header: ambiguous framing, not repaired" :
                            "first WAL frame header invalid");
            inspect_predict(state, TIMELITE_INVALID_DATABASE, "first wal frame");
            return;
        }
        state->wal_first_sequence = inspect_decode64(inspect_bytes + 8);
        if (state->wal_first_sequence != 0 && state->wal_first_sequence <= state->installed)
        {
            state->stale = 1;
            state->sequence = state->wal_first_sequence - 1;
        }
    }
    fprintf(out, "wal_stale=%d\n", state->stale);
    while (offset < state->wal.size)
    {
        error = inspect_frame_parse(&state->wal, offset, state->wal.size, state->sequence + 1,
                                    inspect_bytes, &frame);
        if (error == TIMELITE_END && !state->stale)
        {
            fprintf(out, "wal_incomplete_suffix_offset=%" PRIu64 "\n", offset);
            fprintf(out, "wal_incomplete_suffix_bytes=%" PRIu64 "\n", state->wal.size - offset);
            break;
        }
        if (error != 0)
        {
            inspect_problem(state, offset, "wal", error == TIMELITE_END ?
                            "incomplete frame inside a stale WAL" : frame.reason);
            inspect_predict(state, error == EIO ? EIO : TIMELITE_INVALID_DATABASE, "wal frame");
            break;
        }
        if (state->wal_frames == 0)
        {
            state->wal_first_time = frame.first_time;
        }
        else if (frame.first_time < state->wal_last_time)
        {
            state->wal_ordered = 0;
        }
        state->wal_ordered = state->wal_ordered && frame.ordered;
        state->wal_last_time = frame.last_time;
        if (!state->stale)
        {
            inspect_count_frame(state, &frame);
        }
        state->wal_frames++;
        offset += frame.length;
        state->sequence++;
    }
    state->wal_end = offset;
    fprintf(out, "wal_frames=%" PRIu64 "\n", state->wal_frames);
    if (state->wal_frames != 0)
    {
        fprintf(out, "wal_first_sequence=%" PRIu64 "\n", state->wal_first_sequence);
        fprintf(out, "wal_last_sequence=%" PRIu64 "\n", state->sequence);
        fprintf(out, "wal_first_timestamp_us=%" PRIu64 "\n", state->wal_first_time);
        fprintf(out, "wal_last_timestamp_us=%" PRIu64 "\n", state->wal_last_time);
        fprintf(out, "wal_ordered=%d\n", state->wal_ordered);
    }
    fprintf(out, "wal_logical_end=%" PRIu64 "\n", state->wal_end);
    if (state->predicted_error != 0)
    {
        return;
    }
    if (state->stale && state->sequence != state->installed)
    {
        inspect_problem(state, state->wal_end, "wal", "stale WAL does not end exactly at the installed sequence");
        inspect_predict(state, TIMELITE_INVALID_DATABASE, "stale wal");
    }
    if (state->database.size > state->data_end && state->phase == 0 &&
        !(state->retention && state->generation == 0 && state->database.size == 161) &&
        (state->stale || state->sequence == state->installed))
    {
        inspect_problem(state, state->data_end, "database",
                        "bytes beyond data_end without pending WAL frames: lost newest manifest or externally cut file");
        inspect_predict(state, TIMELITE_INVALID_DATABASE, "orphan bytes");
    }
    if (!state->stale && state->sequence > state->installed)
    {
        state->floor_time = state->wal_last_time;
        state->floor_known = 1;
        state->floor_source = "wal";
        if (state->live == 0)
        {
            state->first_live = state->installed + 1;
        }
    }
}

/* Totals, trailing bytes, and the recovery the library would perform. The
 * predicted.* keys match the fields status prints. */
static void inspect_summary(struct inspect_state *state)
{
    FILE *out = state->out;
    uint64_t pending = state->stale ? 0 : state->sequence - state->installed;
    uint64_t trailing = state->database.size > state->data_end ? state->database.size - state->data_end : 0;
    uint64_t end = state->data_end, wal_end = state->stale ? 32 : state->wal_end;
    int empty_marker = state->retention && state->generation == 0 && state->database.size == 161 &&
                       state->phase == 0;
    fprintf(out, "pending_batches=%" PRIu64 "\n", pending);
    fprintf(out, "committed_batches=%" PRIu64 "\n", state->live + pending);
    fprintf(out, "sequence_high_water=%" PRIu64 "\n", state->stale ? state->installed : state->sequence);
    if (state->live + pending != 0)
    {
        fprintf(out, "first_live_sequence=%" PRIu64 "\n", state->first_live);
    }
    fprintf(out, "records=%" PRIu64 "\n", state->records);
    if (state->have_times)
    {
        fprintf(out, "timestamp_min_us=%" PRIu64 "\n", state->time_min);
        fprintf(out, "timestamp_max_us=%" PRIu64 "\n", state->time_max);
    }
    if (state->floor_known)
    {
        fprintf(out, "timestamp_floor_us=%" PRIu64 "\n", state->floor_time);
        fprintf(out, "timestamp_floor_source=%s\n", state->floor_source);
    }
    fprintf(out, "database_trailing_bytes=%" PRIu64 "\n", empty_marker ? 0 : trailing);
    if (empty_marker)
    {
        fprintf(out, "database_marker_byte=1 (an empty retained database is kept at 161 bytes)\n");
    }
    else if (trailing != 0)
    {
        fprintf(out, "database_trailing_kind=%s\n", state->phase == 0 ?
                "orphan bytes of an interrupted install; ignored and overwritten by the next checkpoint" :
                "unfinished retention copy beyond the authoritative end");
    }
    if (state->root != INSPECT_DATA_START)
    {
        fprintf(out, "database_superseded_front_bytes=%" PRIu64 "\n", state->root - INSPECT_DATA_START);
    }
    fprintf(out, "wal_trailing_bytes=%" PRIu64 "\n", state->wal.size - state->wal_end);
    if (state->unreadable != 0)
    {
        fprintf(out, "first_unreadable_sequence=%" PRIu64 "\n", state->unreadable);
    }

    fprintf(out, "recovery.result=ok\n");
    fprintf(out, "recovery.manifest_slot=%s\n", state->active == NULL ? "none" :
            state->active == &state->slots[0] ? "0" : "1");
    if (state->stale)
    {
        fprintf(out, "recovery.wal_truncate_to=32\n");
        fprintf(out, "recovery.wal_action=finish interrupted reclaim: every WAL frame is already installed\n");
    }
    else if (state->wal_end < state->wal.size)
    {
        fprintf(out, "recovery.wal_truncate_to=%" PRIu64 "\n", state->wal_end);
        fprintf(out, "recovery.wal_action=truncate the incomplete final frame; no committed batch is lost\n");
    }
    else
    {
        fprintf(out, "recovery.wal_action=none\n");
    }
    if (state->phase == 1)
    {
        fprintf(out, "recovery.retention_action=PREPARE: discard the unfinished tail beyond data_end, truncate, publish NORMAL\n");
    }
    else if (state->phase == 2)
    {
        end = INSPECT_DATA_START + (state->data_end - state->root);
        fprintf(out, "recovery.retention_action=TAIL: copy the retained tail back to offset 160, publish CLEANUP, truncate, publish NORMAL\n");
    }
    else if (state->phase == 3)
    {
        fprintf(out, "recovery.retention_action=CLEANUP: truncate to the front copy end, publish NORMAL\n");
    }
    else
    {
        fprintf(out, "recovery.retention_action=none\n");
    }
    if (state->phase != 0)
    {
        /* An empty retained database keeps one marker byte (feature 010). */
        fprintf(out, "recovery.database_truncate_to=%" PRIu64 "\n", end == INSPECT_DATA_START ? 161 : end);
    }
    if (state->unreadable != 0)
    {
        fprintf(out, "recovery.read_fails_at_sequence=%" PRIu64 "\n", state->unreadable);
    }
    fprintf(out, "predicted.committed_batches=%" PRIu64 "\n", state->live + pending);
    fprintf(out, "predicted.installed_batches=%" PRIu64 "\n", state->live);
    fprintf(out, "predicted.pending_batches=%" PRIu64 "\n", pending);
    fprintf(out, "predicted.installed_segments=%" PRIu64 "\n", state->generation);
    fprintf(out, "predicted.wal_bytes=%" PRIu64 "\n", wal_end);
    fprintf(out, "predicted.installed_bytes=%" PRIu64 "\n", end - INSPECT_DATA_START);
    if (state->floor_known)
    {
        fprintf(out, "predicted.last_timestamp_us=%" PRIu64 "\n", state->floor_time);
    }
}

/* Read-only verification of one pair. Returns the exit code. */
static int inspect_verify(const char *database_path, const char *wal_path, FILE *out)
{
    static struct inspect_state state;
    int code;
    memset(&state, 0, sizeof(state));
    state.out = out;
    state.data_end = INSPECT_DATA_START;
    state.root = INSPECT_DATA_START;
    state.floor_source = "none";
    fprintf(out, "mode=verify\n");
    fprintf(out, "read_only=1\n");
    fprintf(out, "note=verify parses the files with stdio only; it never writes, truncates, syncs or creates anything\n");
    fprintf(out, "database=%s\n", database_path);
    fprintf(out, "wal=%s\n", wal_path);
    code = inspect_open(&state.database, database_path, "database", out);
    if (code != 0)
    {
        return code;
    }
    code = inspect_open(&state.wal, wal_path, "wal", out);
    if (code != 0)
    {
        fclose(state.database.file);
        return code;
    }
    fprintf(out, "database_size=%" PRIu64 "\n", state.database.size);
    fprintf(out, "wal_size=%" PRIu64 "\n", state.wal.size);
    inspect_headers(&state);
    if (state.predicted_error == 0 && !state.io_error)
    {
        inspect_manifests(&state);
    }
    if (state.predicted_error == 0 && !state.io_error)
    {
        inspect_segments(&state);
    }
    if (state.predicted_error == 0 && !state.io_error)
    {
        inspect_wal(&state);
    }
    if (state.io_error)
    {
        code = 2;
    }
    else if (state.predicted_error == 0)
    {
        inspect_summary(&state);
        code = state.problems != 0 ? 1 : 0;
    }
    else
    {
        fprintf(out, "recovery.result=%s\n", inspect_error_name(state.predicted_error));
        fprintf(out, "recovery.fails_at=%s\n", state.predicted_stage);
        fprintf(out, "recovery.action=none: the library fails closed and changes neither file\n");
        code = 1;
    }
    fprintf(out, "inconsistencies=%u\n", state.problems);
    fclose(state.database.file);
    fclose(state.wal.file);
    return code;
}

/* --- status: public API only ------------------------------------------- */

/* Opening performs the library's normal recovery. */
static int inspect_status(const char *database_path, const char *wal_path, int dump,
                          const struct timelite_range *range, int have_from, FILE *out)
{
    static unsigned char scratch[TIMELITE_BATCH_SCRATCH];
    static struct timelite_record output[TIMELITE_MAX_RECORDS];
    struct timelite_batches db;
    struct timelite_batches_status status;
    uint64_t sequence;
    size_t count, i;
    int error, close_error, code = 0;
    fprintf(out, "mode=status\n");
    fprintf(out, "read_only=0\n");
    fprintf(out, "note=status opens the pair with the library; open performs normal recovery (WAL suffix truncation, reclaim or retention finish)\n");
    fprintf(out, "database=%s\n", database_path);
    fprintf(out, "wal=%s\n", wal_path);
    (void)timelite_batches_init(&db);
    error = timelite_batches_open(&db, database_path, wal_path, TIMELITE_OPEN_EXISTING,
                                  scratch, sizeof(scratch));
    if (error != 0)
    {
        fprintf(out, "open_error=%s\n", inspect_error_name(error));
        fprintf(out, "open_code=%d\n", error);
        return error < 0 ? 1 : 2;
    }
    fprintf(out, "open_result=ok\n");
    error = timelite_batches_get_status(&db, &status);
    if (error == 0)
    {
        fprintf(out, "committed_batches=%" PRIu64 "\n", status.committed_batches);
        fprintf(out, "installed_batches=%" PRIu64 "\n", status.installed_batches);
        fprintf(out, "pending_batches=%" PRIu64 "\n", status.pending_batches);
        fprintf(out, "installed_segments=%" PRIu64 "\n", status.installed_segments);
        fprintf(out, "wal_bytes=%" PRIu64 "\n", status.wal_bytes);
        fprintf(out, "installed_bytes=%" PRIu64 "\n", status.installed_bytes);
        fprintf(out, "last_timestamp_us=%" PRIu64 "\n", status.last_timestamp_us);
    }
    else
    {
        fprintf(out, "status_error=%s\n", inspect_error_name(error));
    }
    if (error == 0 && dump)
    {
        if (have_from)
        {
            error = timelite_batches_seek(&db, range->from_us, scratch, sizeof(scratch));
            if (error == TIMELITE_END)
            {
                error = 0;
            }
        }
        while (error == 0)
        {
            if (range != NULL)
            {
                error = timelite_batches_next_range(&db, range, output, TIMELITE_MAX_RECORDS,
                                                    &count, &sequence, scratch, sizeof(scratch));
            }
            else
            {
                error = timelite_batches_next(&db, output, TIMELITE_MAX_RECORDS, &count,
                                              &sequence, scratch, sizeof(scratch));
            }
            for (i = 0; error == 0 && i < count; i++)
            {
                fprintf(out, "batch=%" PRIu64 " series=%" PRIu32 " us=%" PRIu64 " value=%" PRId64 "\n",
                        sequence, output[i].series, output[i].timestamp_us, output[i].value);
            }
        }
        if (error == TIMELITE_END)
        {
            error = 0;
        }
        else
        {
            fprintf(out, "read_error=%s\n", inspect_error_name(error));
        }
    }
    close_error = timelite_batches_close(&db);
    if (error != 0)
    {
        code = error < 0 ? 1 : 2;
    }
    else if (close_error != 0)
    {
        fprintf(out, "close_error=%s\n", inspect_error_name(close_error));
        code = 2;
    }
    return code;
}

/* --- command line ------------------------------------------------------- */

static void inspect_usage(FILE *err)
{
    fputs("usage: timelite-inspect verify DATABASE WAL\n"
          "       timelite-inspect status DATABASE WAL [--dump] [--from-us N] [--until-us N] [--series N]\n"
          "\n"
          "verify  read-only: parses both files with stdio, never opens them through\n"
          "        the library, never writes, truncates, syncs or creates anything.\n"
          "        Reports the on-disk layout and predicts what recovery would do.\n"
          "status  opens the pair with the public API (TIMELITE_OPEN_EXISTING). This\n"
          "        performs the library's normal recovery: an incomplete WAL suffix is\n"
          "        truncated, an interrupted reclaim or retention is finished.\n"
          "        --dump prints one line per record; --from-us/--until-us select a\n"
          "        half-open window and --series one series.\n"
          "exit codes: 0 coherent, 1 inconsistency found, 2 usage or I/O error\n", err);
}

static int inspect_number(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    if (text == NULL || text[0] == '\0' || text[0] == '-')
    {
        return 0;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0')
    {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

/* Program entry with explicit streams so the test can compile it in. */
static int timelite_inspect_run(int argc, char **argv, FILE *out, FILE *err)
{
    struct timelite_range range;
    uint64_t series = 0;
    int dump = 0, have_from = 0, have_until = 0, have_series = 0, i;
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        inspect_usage(out);
        return 0;
    }
    if (argc < 4)
    {
        inspect_usage(err);
        return 2;
    }
    if (strcmp(argv[1], "verify") == 0)
    {
        if (argc != 4)
        {
            inspect_usage(err);
            return 2;
        }
        return inspect_verify(argv[2], argv[3], out);
    }
    if (strcmp(argv[1], "status") != 0)
    {
        inspect_usage(err);
        return 2;
    }
    range.from_us = 0;
    range.until_us = UINT64_MAX;
    range.series = 0;
    range.filter_series = 0;
    for (i = 4; i < argc; i++)
    {
        if (strcmp(argv[i], "--dump") == 0)
        {
            dump = 1;
        }
        else if (strcmp(argv[i], "--from-us") == 0 && i + 1 < argc &&
                 inspect_number(argv[i + 1], &range.from_us))
        {
            have_from = 1;
            i++;
        }
        else if (strcmp(argv[i], "--until-us") == 0 && i + 1 < argc &&
                 inspect_number(argv[i + 1], &range.until_us))
        {
            have_until = 1;
            i++;
        }
        else if (strcmp(argv[i], "--series") == 0 && i + 1 < argc &&
                 inspect_number(argv[i + 1], &series) && series <= UINT32_MAX)
        {
            range.series = (uint32_t)series;
            range.filter_series = 1;
            have_series = 1;
            i++;
        }
        else
        {
            inspect_usage(err);
            return 2;
        }
    }
    if ((have_from || have_until || have_series) && !dump)
    {
        fputs("error=--from-us, --until-us and --series require --dump\n", err);
        return 2;
    }
    if (range.from_us > range.until_us)
    {
        fputs("error=--from-us must not exceed --until-us\n", err);
        return 2;
    }
    return inspect_status(argv[2], argv[3], dump,
                          have_from || have_until || have_series ? &range : NULL, have_from, out);
}

#ifndef TIMELITE_INSPECT_NO_MAIN
int main(int argc, char **argv)
{
    return timelite_inspect_run(argc, argv, stdout, stderr);
}
#endif
