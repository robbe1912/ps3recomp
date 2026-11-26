/* ps3recomp - Kernel condition variables.
 *
 * The kernel commits a waiter before releasing its guest mutex. A host CV
 * alone does not retain a signal delivered between syscall entry and parking.
 * Keep explicit wait records under a separate lock; signals never need the
 * guest mutex, and a later waiter cannot steal an earlier waiter's wakeup.
 */
#include "sys_cond.h"
#include "ppu_wait_profile.h"
#include "../memory/vm.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
/* RD_PROBE_THREADSTATES: blocked-on tagging (sys_ppu_thread.c) */
extern void ppu_thread_wait_begin(ppu_context* ctx, const char* kind, uint64_t a, uint64_t b);
extern void ppu_thread_wait_end(ppu_context* ctx);

struct sys_cond_waiter {
    struct sys_cond_waiter* next;
    int signalled;
};

sys_cond_info g_sys_conds[SYS_COND_MAX];
uint32_t g_cond_id_addr[SYS_COND_MAX + 1];

#ifdef _WIN32
static SRWLOCK s_table_lock = SRWLOCK_INIT;
static void table_lock(void) { AcquireSRWLockExclusive(&s_table_lock); }
static void table_unlock(void) { ReleaseSRWLockExclusive(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { EnterCriticalSection(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { LeaveCriticalSection(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { WakeAllConditionVariable(&c->cv); }
static void guest_lock(sys_mutex_info* m) { EnterCriticalSection(&m->cs); }
static void guest_unlock(sys_mutex_info* m) { LeaveCriticalSection(&m->cs); }
#else
static pthread_mutex_t s_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void table_lock(void) { pthread_mutex_lock(&s_table_lock); }
static void table_unlock(void) { pthread_mutex_unlock(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { pthread_mutex_lock(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { pthread_mutex_unlock(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { pthread_cond_broadcast(&c->cv); }
static void guest_lock(sys_mutex_info* m) { pthread_mutex_lock(&m->mtx); }
static void guest_unlock(sys_mutex_info* m) { pthread_mutex_unlock(&m->mtx); }
#endif
/* Bounded failure probe for instant-return sys_cond_wait calls.
 * MotorStorm fn 0x26C798 (VideoStream post/input pump) retries its inner
 * post-wait loop with no sleep: each failed cond_wait costs one full
 * syscall dispatch, so a never-parking waiter spins at ~3M dispatches/s.
 * Name the failing branch in one boot: 16 detail lines, then at most one
 * summary line per 5 minutes. */
static struct {
    long esrch_cond, esrch_mutex, eperm;
    long detail;
    unsigned long long last_summary_ms;
} s_cw_fail;

static unsigned long long cw_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return (unsigned long long)time(NULL) * 1000ull;
#endif
}

static void cond_wait_fail(long* counter, const char* name, uint32_t cond_id,
                           uint32_t mutex_id, sys_mutex_info* m, ppu_context* ctx)
{
    ++*counter;
    if (s_cw_fail.detail < 16) {
        s_cw_fail.detail++;
        fprintf(stderr, "[CWFAIL] %s cond=%u mutex=%u mutex_active=%d "
                        "mutex_owner=%llu lock_count=%d caller_tid=%llu\n",
                name, cond_id, mutex_id,
                m ? (int)m->active : -1,
                m ? (unsigned long long)m->owner_tid : 0ull,
                m ? (int)m->lock_count : -1,
                ctx ? (unsigned long long)ctx->thread_id : 0ull);
    }
    unsigned long long now = cw_now_ms();
    if (now - s_cw_fail.last_summary_ms >= 300000ull) {
        s_cw_fail.last_summary_ms = now;
        fprintf(stderr, "[CWFAIL] summary esrch_cond=%ld esrch_mutex=%ld eperm=%ld\n",
                s_cw_fail.esrch_cond, s_cw_fail.esrch_mutex, s_cw_fail.eperm);
    }
}

/* Lock ordering is table -> condition. No caller holds the condition lock
 * while acquiring a guest mutex. Native locks survive slot recycling. */
static sys_cond_info* acquire_cond(uint32_t id)
{
    if (!id || id > SYS_COND_MAX) return NULL;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return NULL; }
    cond_lock(c);
    table_unlock();
    return c;
}

int64_t sys_cond_create(ppu_context* ctx)
{
    uint32_t out = LV2_ARG_PTR(ctx, 0);
    uint32_t mid = LV2_ARG_U32(ctx, 1);
    uint32_t attr = LV2_ARG_PTR(ctx, 2);
    if (!mid || mid > SYS_MUTEX_MAX || !g_sys_mutexes[mid - 1].active)
        return (int32_t)CELL_ESRCH;
    table_lock();
    for (unsigned i = 0; i < SYS_COND_MAX; ++i) {
        sys_cond_info* c = &g_sys_conds[i];
        if (c->active) continue;
        if (!c->initialized) {
#ifdef _WIN32
            InitializeCriticalSection(&c->signal_lock);
            InitializeConditionVariable(&c->cv);
#else
            int rc = pthread_mutex_init(&c->signal_lock, NULL);
            if (rc) { table_unlock(); return (int32_t)CELL_EAGAIN; }
            rc = pthread_cond_init(&c->cv, NULL);
            if (rc) {
                pthread_mutex_destroy(&c->signal_lock);
                table_unlock(); return (int32_t)CELL_EAGAIN;
            }
#endif
            c->initialized = 1;
        }
        c->mutex_id = mid;
        c->waiters = NULL;
        memset(c->name, 0, sizeof(c->name));
        if (attr) memcpy(c->name, (uint8_t*)vm_to_host(attr) + 8, 8);
        c->active = 1;
        g_cond_id_addr[i + 1] = out;
        if (out) {
            uint8_t* p = vm_to_host(out);
            uint32_t id = i + 1;
            p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
            p[2] = (uint8_t)(id >> 8); p[3] = (uint8_t)id;
        }
        if (getenv("PS3_SYNCLOG") || getenv("YDKJ_SYNCLOG")) {
            char name[9]; memcpy(name, c->name, 8); name[8] = 0;
            fprintf(stderr, "[SYNC] cond_create id=%u name='%s' mutex=%u id_at=0x%08X lr=0x%08X\n",
                    i + 1, name, mid, out, (uint32_t)ctx->lr);
        }
        table_unlock();
        return CELL_OK;
    }
    table_unlock();
    return (int32_t)CELL_EAGAIN;
}

int64_t sys_cond_destroy(ppu_context* ctx)
{
    uint32_t id = LV2_ARG_U32(ctx, 0);
    if (!id || id > SYS_COND_MAX) return (int32_t)CELL_ESRCH;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return (int32_t)CELL_ESRCH; }
    cond_lock(c);
    int32_t result = CELL_OK;
    if (c->waiters) result = (int32_t)CELL_EBUSY;
    else { c->active = 0; g_cond_id_addr[id] = 0; }
    cond_unlock(c);
    table_unlock();
    return result;
}

static int64_t sys_cond_wait_impl(ppu_context* ctx)
{
    /* Firmware ABI (2 args): sys_cond_wait(cond_id r3, timeout_usec r4);
     * r4=0 means wait forever. There is no mutex argument -- the cond's
     * mutex is the one bound at cond_create. Reading the timeout from r5
     * (a 3-arg premise) turned every guest "wait forever" (r4=0, r5 stale)
     * into a finite timeout: ETIMEDOUT wake loops. Proven by lifted caller
     * func_00B9F990 (MotorStorm BCES00006 dequeuer): r3=[obj+0xC] cond,
     * r4=0, r5 never written. */
    uint64_t timeout = LV2_ARG_U64(ctx, 1);
    /* CWENTRY=1: cap-16 entry trace for tid=1 -- cond id, raw r4/r5, and the
     * effective timeout. Discriminates stale-r5 artifacts from real waits. */
    { static int s_on = -1;
      if (s_on < 0) s_on = getenv("CWENTRY") ? 1 : 0;
      if (s_on && ctx->thread_id == 1) {
          static int _n = 0;
          if (_n++ < 16)
              fprintf(stderr, "[CWENTRY] tid=1 cond=%u r4=%llu r5=%llu timeout=%llu%s\n",
                      LV2_ARG_U32(ctx, 0),
                      (unsigned long long)LV2_ARG_U64(ctx, 1),
                      (unsigned long long)LV2_ARG_U64(ctx, 2),
                      (unsigned long long)timeout,
                      timeout ? "" : " (forever)");
      } }
    /* Preserve upstream's opt-in wait diagnostics across the waiter rewrite. */
    const char* peek = getenv("PS3_COND_PEEK");
    if (peek) {
        uint32_t ea = (uint32_t)strtoul(peek, NULL, 16);
        if (ea) {
            const uint8_t* p = vm_to_host(ea);
            uint32_t value = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8) | p[3];
            fprintf(stderr, "[PEEK] [0x%08X]=%u (0x%08X) at cond_wait(cond=%u)\n",
                    ea, value, value, LV2_ARG_U32(ctx, 0));
        }
    }
#ifdef _WIN32
    const char* spurious = getenv("PS3_COND_SPURIOUS_MS");
    long probe_ms = spurious ? atol(spurious) : 0;
    if (probe_ms > 0 && !timeout) timeout = (uint64_t)probe_ms * 1000;
#endif
    if (timeout > ((1ull << 48) - 1)) timeout = (1ull << 48) - 1;
    sys_cond_info* c = acquire_cond(LV2_ARG_U32(ctx, 0));
    if (!c) {
        cond_wait_fail(&s_cw_fail.esrch_cond, "esrch_cond",
                       LV2_ARG_U32(ctx, 0), 0, NULL, ctx);
        return (int32_t)CELL_ESRCH;
    }
    sys_mutex_info* m = &g_sys_mutexes[c->mutex_id - 1];
    if (!m->active) {
        cond_wait_fail(&s_cw_fail.esrch_mutex, "esrch_mutex",
                       LV2_ARG_U32(ctx, 0), c->mutex_id, m, ctx);
        cond_unlock(c); return (int32_t)CELL_ESRCH;
    }
    if (m->owner_tid != ctx->thread_id || m->lock_count <= 0) {
        cond_wait_fail(&s_cw_fail.eperm, "eperm",
                       LV2_ARG_U32(ctx, 0), c->mutex_id, m, ctx);
        cond_unlock(c); return (int32_t)CELL_EPERM;
    }
    struct sys_cond_waiter waiter = {NULL, 0};
    struct sys_cond_waiter** tail = &c->waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &waiter;
    int depth = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;
    for (int i = 0; i < depth; ++i) guest_unlock(m);

    int32_t result = CELL_OK;
    ppu_thread_wait_begin(ctx, "cond_wait", LV2_ARG_U32(ctx, 0), timeout);
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
    uint64_t duration = (timeout + 999) / 1000;
    while (!waiter.signalled) {
        DWORD ms = INFINITE;
        if (timeout) {
            uint64_t elapsed = GetTickCount64() - start;
            if (elapsed >= duration) { result = (int32_t)CELL_ETIMEDOUT; break; }
            uint64_t remaining = duration - elapsed;
            ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
        }
        BOOL ok = SleepConditionVariableCS(&c->cv, &c->signal_lock, ms);
        if (!ok && GetLastError() != ERROR_TIMEOUT && !waiter.signalled) {
            result = (int32_t)CELL_EFAULT; break;
        }
    }
#else
    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(timeout / 1000000);
        deadline.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++; deadline.tv_nsec -= 1000000000L;
        }
    }
    while (!waiter.signalled) {
        int rc = timeout ? pthread_cond_timedwait(&c->cv, &c->signal_lock, &deadline)
                         : pthread_cond_wait(&c->cv, &c->signal_lock);
        if (rc && !waiter.signalled) {
            result = (int32_t)(rc == ETIMEDOUT ? CELL_ETIMEDOUT : CELL_EFAULT);
            break;
        }
    }
#endif
    ppu_thread_wait_end(ctx);
    struct sys_cond_waiter** link = &c->waiters;
    while (*link != &waiter) link = &(*link)->next;
    *link = waiter.next;
    cond_unlock(c);
    for (int i = 0; i < depth; ++i) guest_lock(m);
    m->owner_tid = ctx->thread_id;
    m->lock_count = depth;
    /* CWENTRY pairing: the exit side of the entry trace (same env, own cap).
     * Order pairs the two lines on a single-threaded caller; ETIMEDOUT on a
     * call whose entry line said "(forever)" is the stale-arg smoking gun. */
    { static int s_on = -1;
      if (s_on < 0) s_on = getenv("CWENTRY") ? 1 : 0;
      if (s_on && ctx->thread_id == 1) {
          static int _n = 0;
          if (_n++ < 16)
              fprintf(stderr, "[CWENTRY] tid=1 cond=%u ret=%lld\n",
                      LV2_ARG_U32(ctx, 0), (long long)result);
      } }
    return result;
}

/* Profiled entry: one PPU_WAIT_PROFILE line per call (see ppu_wait_profile.h). */
int64_t sys_cond_wait(ppu_context* ctx)
{
    PPU_WAIT_PROFILE_BEGIN(ctx, "cond_wait", LV2_ARG_U32(ctx, 0));
    int64_t result = sys_cond_wait_impl(ctx);
    ppu_wait_profile_end();
    return result;
}

static int64_t signal_cond(uint32_t id, int all)
{
    sys_cond_info* c = acquire_cond(id);
    if (!c) return (int32_t)CELL_ESRCH;
    int selected = 0;
    for (struct sys_cond_waiter* w = c->waiters; w; w = w->next) {
        if (w->signalled) continue;
        w->signalled = 1;
        selected = 1;
        if (!all) break;
    }
    /* Broadcast the host CV: only selected records return to the guest.
     * Waking an arbitrary host waiter could leave the selected one asleep. */
    if (selected) cond_wake(c);
    cond_unlock(c);
    return CELL_OK;
}

int64_t sys_cond_signal(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 0); }
int64_t sys_cond_signal_all(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 1); }

/* Preserve upstream's current broadcast fallback for signal_to. */
int64_t sys_cond_signal_to(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 1); }

void sys_cond_init(lv2_syscall_table* tbl)
{
    /* Static storage starts empty. Registration must not memset live locks. */
    lv2_syscall_register(tbl, SYS_COND_CREATE, sys_cond_create);
    lv2_syscall_register(tbl, SYS_COND_DESTROY, sys_cond_destroy);
    lv2_syscall_register(tbl, SYS_COND_WAIT, sys_cond_wait);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL, sys_cond_signal);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_ALL, sys_cond_signal_all);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_TO, sys_cond_signal_to);
}
