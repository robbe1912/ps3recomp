#include "rsx_vertex_fetch.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define WORKERS 8
#define CHECK(value, message) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s\n", message); exit(2); } } while (0)

u8* vm_base;
static u8 bytes[8] = {40, 41, 42, 43};
static rsx_state state;
static _Thread_local unsigned worker_id;
static const int* caches[WORKERS];
static atomic_uint ready, finished, start, release;

u32 cellGcmResolveOffset(u32 offset) { (void)offset; abort(); }
u32 cellGcmResolveLocated(int location, u32 offset) {
    CHECK(location == 1 && offset < sizeof bytes, "live element mapping");
    return offset;
}

/* The runner inserts this observation into the actual selector helper. */
void vertex_test_nofreq(const int* cache) {
    CHECK(!caches[worker_id] || caches[worker_id] == cache,
          "frequency selector changes storage within worker");
    caches[worker_id] = cache;
}

static void yield_worker(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

#ifdef _WIN32
static DWORD WINAPI worker(void* argument)
#else
static void* worker(void* argument)
#endif
{
    worker_id = (unsigned)(size_t)argument;
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&start, memory_order_acquire)) yield_worker();

    rsx_vertex_fetch_batch batch;
    rsx_vertex_value prepared[16];
    float scalar[4];
    const int expected = getenv("VP_NOFREQ") != NULL;
    rsx_vertex_fetch_prepare(&batch, &state);
    CHECK(batch.nofreq == expected, "prepared frequency selector");
    rsx_fetch_vertex_prepared(&batch, 3, prepared);
    rsx_fetch_attrib(&state, 0, 3, scalar);
    CHECK(prepared[0].v[0] == (expected ? 43.0f : 41.0f) &&
          scalar[0] == prepared[0].v[0], "threaded prepared/scalar frequency");

    atomic_fetch_add_explicit(&finished, 1, memory_order_release);
    /* Keep every TLS allocation alive until the main thread compares it. */
    while (!atomic_load_explicit(&release, memory_order_acquire)) yield_worker();
    return 0;
}

int main(void) {
    vm_base = bytes;
    state.vertex_attribs[0].enabled = 1;
    state.vertex_attribs[0].stride = 1;
    state.vertex_attribs[0].size = 1;
    state.vertex_attribs[0].type = 7;
    state.vertex_attribs[0].frequency = 2;
#ifdef _WIN32
    HANDLE threads[WORKERS];
#else
    pthread_t threads[WORKERS];
#endif
    for (unsigned i = 0; i < WORKERS; ++i) {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, worker, (void*)(size_t)i, 0, NULL);
        CHECK(threads[i], "create worker");
#else
        CHECK(pthread_create(&threads[i], NULL, worker, (void*)(size_t)i) == 0,
              "create worker");
#endif
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) != WORKERS)
        yield_worker();
    atomic_store_explicit(&start, 1, memory_order_release);
    while (atomic_load_explicit(&finished, memory_order_acquire) != WORKERS)
        yield_worker();
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(caches[i], "worker observed selector storage");
        for (unsigned j = 0; j < i; ++j)
            CHECK(caches[i] != caches[j],
                  "frequency selector state shared across workers");
    }
    atomic_store_explicit(&release, 1, memory_order_release);
    for (unsigned i = 0; i < WORKERS; ++i) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0,
              "join worker");
        CloseHandle(threads[i]);
#else
        CHECK(pthread_join(threads[i], NULL) == 0, "join worker");
#endif
    }
    puts("PASS eight simultaneous prepared/scalar callers with independent selector state");
    return 0;
}
