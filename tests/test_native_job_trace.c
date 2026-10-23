/* Authored event streams only. Include the real production implementation so
 * the reservation/copy seam can force an unfinished writer deterministically. */
#include "ps3emu/native_job_trace.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static atomic_int pause_writer, reserved, release_writer;
static void before_copy(ps3_job_trace*, uint64_t);
#define PS3_JOB_TRACE_BEFORE_COPY(trace, index) before_copy(trace, index)
#ifndef JOB_TRACE_IMPLEMENTATION
#define JOB_TRACE_IMPLEMENTATION "../runtime/diag/native_job_trace.c"
#endif
#include JOB_TRACE_IMPLEMENTATION

static void yield_thread(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
static void before_copy(ps3_job_trace* trace, uint64_t index)
{
    (void)trace; (void)index;
    if (!atomic_load_explicit(&pause_writer, memory_order_acquire)) return;
    atomic_store_explicit(&reserved, 1, memory_order_release);
    while (!atomic_load_explicit(&release_writer, memory_order_acquire)) yield_thread();
}

#define WORKERS 4u
#define EVENTS 4096u
typedef struct worker { ps3_job_trace* trace; uint32_t id, count; } worker;
#ifdef _WIN32
static DWORD WINAPI produce(LPVOID input)
#else
static void* produce(void* input)
#endif
{
    worker* w = input;
    for (uint32_t i = 0; i < w->count; ++i) {
        ps3_job_trace_event event = {0};
        event.tick = i + 1; event.thread = w->id + 1;
        event.instance = 100 + w->id; event.dispatch = i + 1000;
        event.queue_generation = w->id + 10;
        event.data0 = ((uint64_t)w->id << 32) | i;
        event.data1 = ~event.data0;
        event.kind = 3; event.queue = 0x100 + w->id;
        event.workload = 4; event.lane = w->id; event.marker = i;
        event.pc = 0x200 + i * 4;
        assert(ps3_job_trace_append(w->trace, &event) == PS3_JOB_TRACE_RECORDED);
    }
    return 0;
}
#ifdef _WIN32
typedef HANDLE test_thread;
static test_thread start(worker* w) { HANDLE t = CreateThread(NULL, 0, produce, w, 0, NULL); assert(t); return t; }
static void join(test_thread t) { assert(WaitForSingleObject(t, 15000) == WAIT_OBJECT_0); assert(CloseHandle(t)); }
#else
typedef pthread_t test_thread;
static test_thread start(worker* w) { pthread_t t; assert(!pthread_create(&t, NULL, produce, w)); return t; }
static void join(test_thread t) { assert(!pthread_join(t, NULL)); }
#endif

int main(void)
{
    ps3_job_trace_event untouched = {0}, event = {0};
    untouched.data0 = 123;
    assert(ps3_job_trace_append(NULL, &event) == PS3_JOB_TRACE_DISABLED);
    assert(!ps3_job_trace_create(0, 1000, 1));
    assert(!ps3_job_trace_create(1, 0, 1));
    assert(!ps3_job_trace_create(1, 1000, 0));
    assert(!ps3_job_trace_create(1, 1000, PS3_JOB_TRACE_MAX_RECORDS + 1));
    ps3_job_trace* trace = ps3_job_trace_create(99, 10000000, 4);
    assert(trace);
    assert(ps3_job_trace_append(trace, NULL) == PS3_JOB_TRACE_BAD_ARGUMENT);
    assert(ps3_job_trace_read(trace, 0, &untouched) == 0 && untouched.data0 == 123);
    assert(ps3_job_trace_read(trace, 4, &untouched) == -1);
    ps3_job_trace_info info;
    assert(ps3_job_trace_describe(trace, &info));
    assert(info.attempted == 0 && info.dropped == 0 && info.run_id == 99);
    assert(info.capacity == 4 && info.ticks_per_second == 10000000 && info.version == 1);
    assert(info.records_address && info.record_stride >= sizeof(event) + sizeof(uint64_t));
    assert(info.event_offset >= sizeof(uint64_t));

    /* Reserve an event but hold its owner before copying any payload. */
    atomic_store(&pause_writer, 1);
    worker one = {trace, 0, 1};
    test_thread pending = start(&one);
    while (!atomic_load_explicit(&reserved, memory_order_acquire)) yield_thread();
    assert(ps3_job_trace_describe(trace, &info) && info.attempted == 1);
    assert(ps3_job_trace_read(trace, 0, &untouched) == 0 && untouched.data0 == 123);
    atomic_store_explicit(&release_writer, 1, memory_order_release);
    join(pending);
    atomic_store(&pause_writer, 0);
    assert(ps3_job_trace_read(trace, 0, &event) == 1 && event.data1 == UINT64_MAX);
    ps3_job_trace_destroy(trace);

    trace = ps3_job_trace_create(101, 1000, 3);
    assert(trace);
    for (unsigned i = 0; i < 9; ++i) {
        event = (ps3_job_trace_event){0}; event.data0 = i; event.reserved = 0xffffffffu;
        enum ps3_job_trace_result result = ps3_job_trace_append(trace, &event);
        assert(result == (i < 3 ? PS3_JOB_TRACE_RECORDED : PS3_JOB_TRACE_FULL));
    }
    assert(ps3_job_trace_describe(trace, &info) && info.attempted == 9 && info.dropped == 6);
    for (unsigned i = 0; i < 3; ++i) {
        assert(ps3_job_trace_read(trace, i, &event) == 1 && event.data0 == i);
        assert(event.flags & PS3_JOB_TRACE_CLOCK_INVALID);
        assert(event.reserved == 0);
    }
    ps3_job_trace_destroy(trace);

    trace = ps3_job_trace_create(102, 10000000, WORKERS * EVENTS);
    assert(trace);
    worker workers[WORKERS]; test_thread threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) { workers[i] = (worker){trace, i, EVENTS}; threads[i] = start(&workers[i]); }
    for (unsigned i = 0; i < WORKERS; ++i) join(threads[i]);
    unsigned char* seen = calloc(WORKERS * EVENTS, 1); assert(seen);
    for (unsigned i = 0; i < WORKERS * EVENTS; ++i) {
        assert(ps3_job_trace_read(trace, i, &event) == 1);
        unsigned lane = (unsigned)(event.data0 >> 32), n = (unsigned)event.data0;
        assert(lane < WORKERS && n < EVENTS && !seen[lane * EVENTS + n]++);
        assert(event.data1 == ~event.data0 && event.thread == lane + 1 && event.tick == n + 1);
        assert(event.instance == 100 + lane && event.dispatch == n + 1000);
        assert(event.queue_generation == lane + 10 && event.queue == 0x100 + lane);
        assert(event.workload == 4 && event.lane == lane && event.marker == n && event.pc == 0x200 + 4 * n);
        assert(event.kind == 3 && !event.flags && !event.reserved);
    }
    assert(ps3_job_trace_describe(trace, &info) && info.attempted == WORKERS * EVENTS && !info.dropped);
    free(seen); ps3_job_trace_destroy(trace); ps3_job_trace_destroy(NULL);
    puts("PASS native job trace: unfinished publication, immutable overflow, loss accounting, invalid clock, 16384 concurrent authored events");
    return 0;
}
