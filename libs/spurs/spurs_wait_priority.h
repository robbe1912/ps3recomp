/* spurs_wait_priority.h - SPURS_WAIT_PRIORITY: the workload that a PPU thread
 * waits on gets a free lane first.
 *
 * A PPU thread that waits on a job list cannot continue until the workload of
 * that job list ran. The pass of the coordinator gives each free lane to the
 * first workload that passes the admission checks, from a round-robin cursor.
 * The workload of the waited job list can then wait behind other workloads.
 *
 * With SPURS_WAIT_PRIORITY=1 the port registers a callback with
 * spurs_wait_priority_set(). The callback returns the job list address that a
 * PPU thread waits on, or 0 for none. The pass finds the workload of its
 * instance whose data is that address, and tests that workload at the first
 * position of the scan of each free lane. The admission checks do not change.
 * The other workloads keep the order of the cursor.
 *
 * The coordinator calls the callback under the instance lock. The callback
 * must return at once and must not call a SPURS function.
 *
 * The counters count the free lanes that the pass gave while a PPU thread
 * waited on a workload (waits) and the jobs admitted at the first position
 * (boosted_jobs). The coordinator prints them at most one line per second.
 *
 * Include this header in one translation unit: libs/spurs/cellSpurs.c.
 */
#ifndef PS3EMU_SPURS_WAIT_PRIORITY_H
#define PS3EMU_SPURS_WAIT_PRIORITY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SPURS_WAIT_PRIORITY_WORKLOADS 16
#define SPURS_WAIT_PRIORITY_LINE_MS 1000

/* Returns the job list address that a PPU thread waits on, or 0 for none. */
typedef uint64_t (*spurs_wait_priority_fn)(void *ctx);

/* Reads the data of workload wid for the instance that runs the pass. Returns
 * 0 for a workload that is not in use in that instance. */
typedef uint64_t (*spurs_wait_priority_data_fn)(void *ctx, unsigned wid);

static spurs_wait_priority_fn s_wait_priority_fn;
static void *s_wait_priority_ctx;
static uint64_t s_wait_priority_waits, s_wait_priority_jobs;
static unsigned long s_wait_priority_last_line;

/* SPURS_WAIT_PRIORITY=1, and no other value. */
static int spurs_wait_priority_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SPURS_WAIT_PRIORITY"); v = e && e[0] == '1' && !e[1]; }
    return v;
}

/* Registers the callback. A null function removes it. The function pointer is
 * null while the context changes, so the coordinator never calls a function
 * with the context of another registration. */
void spurs_wait_priority_set(spurs_wait_priority_fn fn, void *ctx)
{
    __atomic_store_n(&s_wait_priority_fn, (spurs_wait_priority_fn)0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_wait_priority_ctx, fn ? ctx : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_wait_priority_fn, fn, __ATOMIC_RELEASE);
}

/* The job list address that a PPU thread waits on, or 0. */
static uint64_t spurs_wait_priority_job_list(void)
{
    if (!spurs_wait_priority_on()) return 0;
    spurs_wait_priority_fn fn = __atomic_load_n(&s_wait_priority_fn, __ATOMIC_ACQUIRE);
    return fn ? fn(__atomic_load_n(&s_wait_priority_ctx, __ATOMIC_ACQUIRE)) : 0;
}

/* The first workload whose data is the waited job list, or -1. */
static int spurs_wait_priority_workload(uint64_t list, spurs_wait_priority_data_fn data, void *ctx)
{
    if (!list) return -1;
    for (unsigned wid = 0; wid < SPURS_WAIT_PRIORITY_WORKLOADS; wid++)
        if (data(ctx, wid) == list) return (int)wid;
    return -1;
}

/* Positions of the scan of one free lane: one for the wanted workload when
 * there is one, then one for each workload of the cursor. */
static unsigned spurs_wait_priority_count(int wanted)
{
    return SPURS_WAIT_PRIORITY_WORKLOADS + (wanted >= 0);
}

/* The workload at position checked of the scan, or -1 when the position is
 * empty. Position 0 is the wanted workload. Each other position takes one
 * workload from the cursor. The position of the wanted workload is empty:
 * the workload had its position. */
static int spurs_wait_priority_at(int wanted, unsigned *cursor, unsigned checked)
{
    if (wanted >= 0 && !checked) return wanted;
    unsigned wid = *cursor;
    *cursor = (wid + 1) & (SPURS_WAIT_PRIORITY_WORKLOADS - 1u);
    return (int)wid == wanted ? -1 : (int)wid;
}

/* One free lane that the pass gave while a PPU thread waited on a workload. */
static void spurs_wait_priority_wait(void)
{
    s_wait_priority_waits++;
}

/* One job that the pass admitted at the position of the wanted workload. */
static void spurs_wait_priority_job(void)
{
    s_wait_priority_jobs++;
}

/* Prints the counters. At most one line per second, and only with the switch. */
static void spurs_wait_priority_report(unsigned long now_ms)
{
    if (!spurs_wait_priority_on() || now_ms - s_wait_priority_last_line < SPURS_WAIT_PRIORITY_LINE_MS)
        return;
    s_wait_priority_last_line = now_ms;
    fprintf(stderr, "[SPURS_WAIT_PRIORITY] waits=%llu boosted_jobs=%llu\n",
            (unsigned long long)s_wait_priority_waits, (unsigned long long)s_wait_priority_jobs);
}

#endif
