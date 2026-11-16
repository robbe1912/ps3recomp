/* Authored concurrent enqueue/service model, with actual production queue/service code. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/guest_read_sync.h"
#include "rsx_cpu_surface_profile.h"

typedef uint32_t u32;
typedef uint64_t u64;
#ifdef _WIN32
#include <windows.h>
static void yield_thread(void) { Sleep(0); }
#else
#include <pthread.h>
#include <sched.h>
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef void *HANDLE;
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define FALSE 0
#define E_FAIL (-1)
typedef struct LARGE_INTEGER { int64_t QuadPart; } LARGE_INTEGER;
static _Thread_local DWORD thread_id;
static DWORD GetCurrentThreadId(void) { return thread_id; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
    __atomic_compare_exchange_n(p, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static void AcquireSRWLockExclusive(SRWLOCK *p) { pthread_mutex_lock(p); }
static void ReleaseSRWLockExclusive(SRWLOCK *p) { pthread_mutex_unlock(p); }
static int QueryPerformanceCounter(LARGE_INTEGER *p) { p->QuadPart = 1; return 1; }
static void yield_thread(void) { sched_yield(); }
#endif

static _Thread_local unsigned locks_held;
static void request_lock(SRWLOCK *lock) { AcquireSRWLockExclusive(lock); ++locks_held; }
static void request_unlock(SRWLOCK *lock) { --locks_held; ReleaseSRWLockExclusive(lock); }
#define AcquireSRWLockExclusive request_lock
#define ReleaseSRWLockExclusive request_unlock
static int signal_request(HANDLE event) {
    atomic_store_explicit((atomic_int *)event, 1, memory_order_release);
    return 1;
}
#define SetEvent signal_request

static int s_cpu_surface_recording;
static unsigned reads, submitted;
static atomic_uint enqueued, finished;
static void cpu_surface_fail(const char *message, int result) {
    fprintf(stderr, "FAIL: %s %d\n", message, result);
    exit(1);
}
/* PRODUCTION_DECLARATIONS */
_Static_assert(_Generic(&s_cpu_surface_head, _Atomic(CpuSurfaceRequest *) *: 1, default: 0),
               "Publish queue head with C11 atomics");
/* PRODUCTION_HELPERS */
static void cpu_surface_read(u32 address, u32 bytes) {
    if (locks_held || s_cpu_surface_recording || bytes != 2)
        cpu_surface_fail("invalid read ownership", 0);
    if (address == 0x10020 && !submitted)
        cpu_surface_fail("read before pending write submission", 0);
    ++reads;
}
/* PRODUCTION_PROMPT */

enum { WORKERS = 4, REQUESTS = 64 };
static void producer(unsigned worker) {
#ifndef _WIN32
    thread_id = worker + 10;
#endif
    for (unsigned i = 0; i < REQUESTS; ++i) {
        atomic_int done = 0;
        CpuSurfaceRequest request;
        memset(&request, 0, sizeof request);
        request.address = i == 0 ? 0x10020 : 0x20000 + worker * 0x1000 + i * 4;
        request.bytes = 2;
        request.status = -1;
        request.done = (HANDLE)&done;
        AcquireSRWLockExclusive(&s_cpu_surface_lock);
        if (s_cpu_surface_tail) s_cpu_surface_tail->next = &request;
        else s_cpu_surface_head = &request;
        s_cpu_surface_tail = &request;
        ReleaseSRWLockExclusive(&s_cpu_surface_lock);
        atomic_fetch_add_explicit(&enqueued, 1, memory_order_release);
        /* A client must never service reads itself. */
        cpu_surface_service_ready();
        while (!atomic_load_explicit(&done, memory_order_acquire)) yield_thread();
        if (request.status != 0) cpu_surface_fail("completion status not published", 0);
    }
    atomic_fetch_add_explicit(&finished, 1, memory_order_release);
}
#ifdef _WIN32
static DWORD WINAPI producer_entry(void *argument) { producer((unsigned)(uintptr_t)argument); return 0; }
#else
static void *producer_entry(void *argument) { producer((unsigned)(uintptr_t)argument); return NULL; }
#endif

int main(void) {
#ifndef _WIN32
    thread_id = 2;
#endif
    s_cpu_surface_thread = (LONG)GetCurrentThreadId();
    s_cpu_surface_prompt = 1;
    cpu_surface_pending_add(0x10000, 0x100, 4);
#ifdef _WIN32
    HANDLE threads[WORKERS];
#else
    pthread_t threads[WORKERS];
#endif
    for (unsigned i = 0; i < WORKERS; ++i) {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, producer_entry, (void *)(uintptr_t)i, 0, NULL);
        if (!threads[i]) cpu_surface_fail("CreateThread", 0);
#else
        if (pthread_create(&threads[i], NULL, producer_entry, (void *)(uintptr_t)i))
            cpu_surface_fail("pthread_create", 0);
#endif
    }
    while (atomic_load_explicit(&enqueued, memory_order_acquire) != WORKERS) yield_thread();
    cpu_surface_service_ready();
    if (reads || atomic_load_explicit(&finished, memory_order_acquire))
        cpu_surface_fail("overlapping requests completed early", 0);
    submitted = 1;
    cpu_surface_batch_submitted();
    while (atomic_load_explicit(&finished, memory_order_acquire) != WORKERS) {
        cpu_surface_service_ready();
        yield_thread();
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    if (reads != WORKERS * REQUESTS || s_cpu_surface_head || s_cpu_surface_tail)
        cpu_surface_fail("lost request or stale queue tail", 0);
    puts("PASS: four concurrent clients, 256 requests, pending-write gate, exact completions, empty queue");
    return 0;
}
