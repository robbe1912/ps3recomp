/* ppu_wait_profile.h - opt-in wall-time record of guest waits.
 *
 * PPU_WAIT_PROFILE=1 writes one line for each ended wait that a syscall
 * wrapper brackets with PPU_WAIT_PROFILE_BEGIN and ppu_wait_profile_end:
 *
 *   [PPU_WAIT_PROFILE] tid=<thread> kind=<name> id=<object> lr=<guest call site> started=<ticks> finished=<ticks> frequency=<ticks per second>
 *
 * The line holds no duration and no result: take the difference of the two
 * ticks. A wait that returns at once also writes a line. A begin on a thread
 * that has an open begin replaces that begin. Without the switch the two
 * functions read one static int and one thread-local int, and print nothing.
 */
#ifndef PPU_WAIT_PROFILE_H
#define PPU_WAIT_PROFILE_H

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#ifdef _MSC_VER
#define PPU_WAIT_THREAD_LOCAL __declspec(thread)
#else
#define PPU_WAIT_THREAD_LOCAL __thread
#endif
typedef struct PpuWaitProfileState {
    unsigned long long tid;
    unsigned long long id;
    unsigned long long start;
    unsigned lr;
    const char* kind;
    int active;
} PpuWaitProfileState;

static PPU_WAIT_THREAD_LOCAL PpuWaitProfileState t_ppu_wait_profile;

static int ppu_wait_profile_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* value = getenv("PPU_WAIT_PROFILE");
        on = (value && value[0] == '1' && value[1] == 0) ? 1 : 0;
    }
    return on;
}

static unsigned long long ppu_wait_profile_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return (unsigned long long)ticks.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec;
#endif
}

static unsigned long long ppu_wait_profile_frequency(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (unsigned long long)frequency.QuadPart;
#else
    return 1000000000ull;
#endif
}

static void ppu_wait_profile_begin(unsigned long long tid, unsigned lr,
                                   const char* kind, unsigned long long id)
{
    if (!ppu_wait_profile_on()) {
        t_ppu_wait_profile.active = 0;
        return;
    }
    t_ppu_wait_profile.tid = tid;
    t_ppu_wait_profile.id = id;
    t_ppu_wait_profile.start = ppu_wait_profile_now();
    t_ppu_wait_profile.lr = lr;
    t_ppu_wait_profile.kind = kind;
    t_ppu_wait_profile.active = 1;
}

static void ppu_wait_profile_end(void)
{
    unsigned long long finished;
    if (!t_ppu_wait_profile.active)
        return;
    t_ppu_wait_profile.active = 0;
    finished = ppu_wait_profile_now();
    fprintf(stderr, "[PPU_WAIT_PROFILE] tid=%llu kind=%s id=%llu lr=%08X started=%llu finished=%llu frequency=%llu\n",
            t_ppu_wait_profile.tid, t_ppu_wait_profile.kind, t_ppu_wait_profile.id,
            t_ppu_wait_profile.lr, t_ppu_wait_profile.start, finished,
            ppu_wait_profile_frequency());
}

/* ctx is the ppu_context of the syscall. The macro reads two fields only, so
 * the check compiles this header without the runtime headers. */
#define PPU_WAIT_PROFILE_BEGIN(ctx, wait_kind, wait_id)                           \
    ppu_wait_profile_begin((unsigned long long)(ctx)->thread_id,                  \
                           (unsigned)(ctx)->lr, (wait_kind),                      \
                           (unsigned long long)(wait_id))

#endif
