#include "ps3emu/native_job_trace.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct ps3_job_trace_record {
    atomic_uint_fast64_t sequence;
    ps3_job_trace_event event;
} ps3_job_trace_record;
struct ps3_job_trace {
    atomic_uint_fast64_t attempted, dropped;
    uint64_t run_id, ticks_per_second;
    uint32_t capacity;
    ps3_job_trace_record* records;
};

/* Fixture interleaving seam: the normal build emits no callback or branch. */
#ifndef PS3_JOB_TRACE_BEFORE_COPY
#define PS3_JOB_TRACE_BEFORE_COPY(trace, index) ((void)0)
#endif

ps3_job_trace* ps3_job_trace_create(uint64_t run_id, uint64_t frequency,
                                    uint32_t capacity)
{
    if (!run_id || !frequency || !capacity || capacity > PS3_JOB_TRACE_MAX_RECORDS)
        return NULL;
    ps3_job_trace* trace = calloc(1, sizeof(*trace));
    if (!trace) return NULL;
    atomic_init(&trace->attempted, 0);
    atomic_init(&trace->dropped, 0);
    if (!atomic_is_lock_free(&trace->attempted) || !atomic_is_lock_free(&trace->dropped)) {
        free(trace);
        return NULL;
    }
    trace->records = calloc(capacity, sizeof(*trace->records));
    if (!trace->records) { free(trace); return NULL; }
    for (uint32_t i = 0; i < capacity; ++i)
        atomic_init(&trace->records[i].sequence, 0);
    if (!atomic_is_lock_free(&trace->records[0].sequence)) {
        free(trace->records);
        free(trace);
        return NULL;
    }
    trace->run_id = run_id;
    trace->ticks_per_second = frequency;
    trace->capacity = capacity;
    return trace;
}

enum ps3_job_trace_result ps3_job_trace_append(ps3_job_trace* trace,
                                               const ps3_job_trace_event* event)
{
    if (!trace) return PS3_JOB_TRACE_DISABLED;
    if (!event) return PS3_JOB_TRACE_BAD_ARGUMENT;
    uint64_t index = atomic_fetch_add_explicit(&trace->attempted, 1, memory_order_relaxed);
    if (index >= trace->capacity) {
        atomic_fetch_add_explicit(&trace->dropped, 1, memory_order_relaxed);
        return PS3_JOB_TRACE_FULL;
    }
    ps3_job_trace_record* record = &trace->records[index];
    PS3_JOB_TRACE_BEFORE_COPY(trace, index);
    record->event = *event;
    record->event.reserved = 0;
    if (!event->tick) record->event.flags |= PS3_JOB_TRACE_CLOCK_INVALID;
    atomic_store_explicit(&record->sequence, index + 1, memory_order_release);
    return PS3_JOB_TRACE_RECORDED;
}

int ps3_job_trace_read(const ps3_job_trace* trace, uint64_t index,
                         ps3_job_trace_event* output)
{
    if (!trace || !output || index >= trace->capacity) return -1;
    const ps3_job_trace_record* record = &trace->records[index];
    if (atomic_load_explicit(&record->sequence, memory_order_acquire) != index + 1)
        return 0;
    *output = record->event;
    return 1;
}

int ps3_job_trace_describe(const ps3_job_trace* trace, ps3_job_trace_info* output)
{
    if (!trace || !output) return 0;
    *output = (ps3_job_trace_info){
        trace->run_id, trace->ticks_per_second,
        atomic_load_explicit(&trace->attempted, memory_order_relaxed),
        atomic_load_explicit(&trace->dropped, memory_order_relaxed),
        (uint64_t)(uintptr_t)trace->records, trace->capacity,
        (uint32_t)sizeof(ps3_job_trace_record),
        (uint32_t)offsetof(ps3_job_trace_record, event), PS3_JOB_TRACE_VERSION
    };
    return 1;
}

void ps3_job_trace_destroy(ps3_job_trace* trace)
{
    if (trace) { free(trace->records); free(trace); }
}
