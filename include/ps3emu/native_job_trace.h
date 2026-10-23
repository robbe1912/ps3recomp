#ifndef PS3EMU_NATIVE_JOB_TRACE_H
#define PS3EMU_NATIVE_JOB_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in diagnostic storage, not a scheduler or completion mechanism.
 * The owner supplies one clock domain and lifetime-qualified identifiers.
 * Zero/unknown identities remain unknown; this layer never invents edges. */
typedef struct ps3_job_trace ps3_job_trace;
enum { PS3_JOB_TRACE_MAX_RECORDS = 1048576, PS3_JOB_TRACE_VERSION = 1 };
enum {
    PS3_JOB_TRACE_CLOCK_INVALID = 1u,
    PS3_JOB_TRACE_IDENTITY_PARTIAL = 2u,
    PS3_JOB_TRACE_DEPENDENCIES_UNKNOWN = 4u
};
typedef struct ps3_job_trace_event {
    uint64_t tick, thread, instance, dispatch, queue_generation, data0, data1;
    uint32_t kind, flags, queue, workload, lane, marker, pc, reserved;
} ps3_job_trace_event;
typedef struct ps3_job_trace_info {
    uint64_t run_id, ticks_per_second, attempted, dropped, records_address;
    uint32_t capacity, record_stride, event_offset, version;
} ps3_job_trace_info;
enum ps3_job_trace_result {
    PS3_JOB_TRACE_DISABLED = 0, PS3_JOB_TRACE_RECORDED = 1,
    PS3_JOB_TRACE_FULL = 2, PS3_JOB_TRACE_BAD_ARGUMENT = 3
};

/* Allocate only at setup. Creation failure must be reported by the owner.
 * No environment access, allocation, formatting, file I/O or clock calls in
 * append/read/describe. Storage never wraps or reuses an event slot. */
ps3_job_trace* ps3_job_trace_create(uint64_t run_id, uint64_t ticks_per_second,
                                    uint32_t capacity);
enum ps3_job_trace_result ps3_job_trace_append(ps3_job_trace*,
                                               const ps3_job_trace_event*);
/* 1 = complete immutable record, 0 = not committed (possibly in flight),
 * -1 = invalid argument. An uncommitted slot is never a zero-valued event. */
int ps3_job_trace_read(const ps3_job_trace*, uint64_t index, ps3_job_trace_event*);
int ps3_job_trace_describe(const ps3_job_trace*, ps3_job_trace_info*);
/* Call only after all producers AND consumers have stopped. Before that,
 * describe/read are live observations, not an atomic final snapshot.
 * An external reader must validate each record's publication sequence and
 * retain missing slots/drop counts. Do not infer a DAG from record order. */
void ps3_job_trace_destroy(ps3_job_trace*);

#ifdef __cplusplus
}
#endif
#endif
