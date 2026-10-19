/* Authored dispatcher-boundary fixture; no guest code or real host threads. */
#include "spu_workload.h"
#include "win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <pthread.h>
#endif

/* ACTUAL_JOB_TYPE */

uint8_t *vm_base;
uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
uint32_t spu_thread_register_phantom(void) { return 17; }
static unsigned s_async_disp[64], s_async_ret[64];
#ifdef LIFECYCLE_API
static void ts_lock(void) {}
static void ts_unlock(void) {}
int spu_task_claim_locked(uint32_t ea, uint8_t* h, uint32_t id, spu_task_owner* o) {
    static uint64_t sequence; (void)h;
    *o = (spu_task_owner){ea, id, 1, ++sequence}; return 1;
}
int spu_task_cancel_locked(const spu_task_owner* o, uint8_t* h) { (void)o; (void)h; return 1; }
#endif
static void entry(spu_context *c) { (void)c; abort(); }
static struct { uint64_t fp; spu_lifted_entry_fn fn; int image_id; }
    s_registry[] = {{42, entry, 1}};
static unsigned s_registry_count = 1;
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* image) {
    for (unsigned i = 0; i < s_registry_count; ++i)
        if (s_registry[i].fp == fp) {
            if (image) *image = s_registry[i].image_id;
            return s_registry[i].fn;
        }
    if (image) *image = 0;
    return NULL;
}
static spu_async_job captured;
static unsigned spawns;
static int refuse_spawn;
uint64_t spu_workload_fingerprint(const void *data, size_t size) {
    const uint8_t *image = data;
    (void)size;
    /* Deterministic legal interleaving: caller B publishes before A captures. */
    g_ydkj_real_taskset_ea = 0x20000;
    g_ydkj_real_taskid = 7;
    return image[0];
}
static unsigned long long loadtime_us(void) { return 0; }
static int loadtime_on(void) { return 0; }
static void loadtime_event(const char *a, const char *b) { (void)a; (void)b; }
static void fixture_setname(uintptr_t t, const char *n) { (void)t; (void)n; }
#define ps3_thread_setname fixture_setname
static void capture_job(void *p) {
    captured = *(spu_async_job *)p;
    spawns++;
    free(p);
}
#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) { (void)p; abort(); }
static HANDLE WINAPI fixture_create(LPSECURITY_ATTRIBUTES a, SIZE_T s,
        LPTHREAD_START_ROUTINE f, LPVOID p, DWORD flags, LPDWORD id) {
    (void)a; (void)s; (void)flags; (void)id;
    if (f != spu_async_thread) abort();
    if (refuse_spawn) return NULL;
    capture_job(p);
    return (HANDLE)1;
}
static BOOL WINAPI fixture_close(HANDLE h) { return h == (HANDLE)1; }
#define CreateThread fixture_create
#define CloseHandle fixture_close
#else
static void *spu_async_thread(void *p) { (void)p; abort(); }
static int fixture_create(pthread_t *t, const pthread_attr_t *a,
        void *(*f)(void *), void *p) {
    (void)a; if (f != spu_async_thread) abort();
    if (refuse_spawn) return EAGAIN;
    capture_job(p); *t = (pthread_t)1; return 0;
}
static int fixture_detach(pthread_t t) { (void)t; return 0; }
#define pthread_create fixture_create
#define pthread_detach fixture_detach
#endif

/* ACTUAL_DISPATCH_APIS */

static int generic(const uint8_t *image) {
#ifdef ASYNC_HAS_ARGUMENT
    return spu_workload_dispatch_async(image, 1, 0, NULL);
#else
    return spu_workload_dispatch_async(image, 1, 0);
#endif
}
static int owned_dispatch(const uint8_t *image, uint32_t ts, uint32_t id) {
    g_ydkj_real_taskset_ea = ts; g_ydkj_real_taskid = id;
#ifdef EXPLICIT_TASK_API
    return spu_workload_dispatch_task(image, 1, 0, NULL, ts, id);
#else
    return generic(image);
#endif
}
int main(void) {
    const uint8_t image[] = {42}, missing[] = {43};
    int failures = 0;
    for (unsigned i = 0; i < 256; i++) {
        unsigned before = spawns;
        if (!owned_dispatch(image, 0x10000, i % 128) || spawns != before + 1 ||
            captured.taskset_ea != 0x10000 || captured.taskid != i % 128) {
            fprintf(stderr, "FAIL explicit identity: iteration=%u got=%08X/%u\n",
                    i, captured.taskset_ea, captured.taskid);
            failures++; break;
        }
    }
    if (!generic(image) || captured.taskset_ea != 0 || captured.taskid != 0) {
        fprintf(stderr, "FAIL generic dispatch inherited another task\n"); failures++;
    }
    unsigned before = spawns;
    if (generic(missing) || spawns != before) { failures++; }
#ifdef EXPLICIT_TASK_API
    if (spu_workload_dispatch_task(image, 1, 0, NULL, 0x10000, 128) ||
        spu_workload_dispatch_task(image, 1, 0, NULL, 0, 7) || spawns != before)
        failures++;
    uint32_t arg[4] = {0x11, 0x22, 0x33, 0x44};
    if (!spu_workload_dispatch_task(image, 1, 0, arg, 0x10000, 3) ||
        !captured.have_r3 || memcmp(captured.r3, arg, sizeof arg)) failures++;
    arg[0] = 0;
    if (captured.r3[0] != 0x11) failures++;
    before = spawns;
#endif
    refuse_spawn = 1;
    if (owned_dispatch(image, 0x10000, 3) || spawns != before) { failures++; }
    printf("task identity fixture failures=%d spawns=%u\n", failures, spawns);
    return failures ? 1 : 0;
}
