/* spurs_notify_wait.h - the wait of the SPURS_KERNEL_NOTIFY coordinator.
 *
 * The coordinator of a SPURS instance gives each free lane to a workload. It
 * runs one pass, then waits for one of two events:
 *
 *   - a kick: a lane returned, or a ReadyCount call stored a nonzero value.
 *     The pass after a kick admits workloads with a ready count or a signal;
 *   - the fallback deadline, SPURS_KERNEL_POLL_MS after the last timed pass.
 *     The pass after a deadline is a timed pass. It admits every eligible
 *     workload. A kick does not move the deadline.
 *
 * Both requirements of that wait failed on a Windows desktop at its default
 * timer resolution:
 *
 *   - The deadline was kept in GetTickCount64. That clock advances with the
 *     system tick, 15.6 ms, so a deadline of 1 ms was reached at a tick edge
 *     only. The timed pass ran about every 15.6 ms.
 *   - The wait was SleepConditionVariableSRW with a timeout in whole
 *     milliseconds. A timeout ends at the next tick when no process raises
 *     the timer resolution. Windows ignores such a request of a process whose
 *     window is minimized or covered.
 *
 * Now the deadline uses QueryPerformanceCounter, which has no step, and the
 * wait uses a manual-reset waitable timer, created with
 * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION where available. The coordinator arms
 * the timer for the deadline. A kick arms it to expire at once. Arming
 * replaces a signal that is pending, so a wait never ends twice per arm.
 *
 * Include this header after SPURS_KERNEL_POLL_MS. The header needs the Win32
 * calls, also on POSIX hosts (runtime/platform/win32_compat.h).
 */
#ifndef SPURS_NOTIFY_WAIT_H
#define SPURS_NOTIFY_WAIT_H

#include <stdint.h>
#include "../../runtime/platform/win32_compat.h"

/* Units of 100 ns in one fallback period. */
#define SPURS_KERNEL_POLL_100NS ((uint64_t)SPURS_KERNEL_POLL_MS * 10000u)

/* Time in 100 ns for a counter that read `counter` ticks at `frequency` ticks
 * per second. 0 when the counter has no rate. */
static inline uint64_t spurs_kernel_100ns(uint64_t counter, uint64_t frequency)
{
    if (!frequency)
        return 0;
    /* Two products: counter * 10000000 does not fit in 64 bits. */
    return counter / frequency * 10000000u + counter % frequency * 10000000u / frequency;
}

/* Monotonic time in units of 100 ns since an arbitrary origin. */
static inline uint64_t spurs_kernel_now(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return spurs_kernel_100ns((uint64_t)counter.QuadPart, (uint64_t)frequency.QuadPart);
}

/* One manual-reset waitable timer. Windows 10 1803 and later end a wait on it
 * below the system tick. Older systems and the POSIX shim keep the tick. */
static inline HANDLE spurs_poll_timer_create(void)
{
#if defined(_WIN32) && _WIN32_WINNT >= 0x0A00
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (timer) return timer;
#endif
    return CreateWaitableTimerW(NULL, TRUE, NULL);
}

/* Make the timer signal when the monotonic time of spurs_kernel_now() reaches
 * `deadline`. A signal that is pending is replaced. A deadline that passed
 * signals at once. 0 on failure. */
static inline int spurs_poll_arm(HANDLE timer, uint64_t deadline)
{
    LARGE_INTEGER due;
    uint64_t now = spurs_kernel_now();
    /* Relative time in 100 ns is negative. */
    due.QuadPart = deadline > now ? -(LONGLONG)(deadline - now) : -1;
    return SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
}

/* Make the timer signal at once. */
static inline void spurs_poll_wake(HANDLE timer)
{
    LARGE_INTEGER due;
    if (!timer) return;
    due.QuadPart = -1;
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
}

#endif
