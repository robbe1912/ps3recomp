/* Authored harness of libs/spurs/spurs_wait_priority.h.
 *
 * Prints one record per rule to stdout. The check runs it with the switch off
 * and with SPURS_WAIT_PRIORITY=1. The report lines go to stderr. */
#include <stdint.h>
#include <stdio.h>
#include "../libs/spurs/spurs_wait_priority.h"

#define RECORD "[wait_priority] "

static uint64_t g_waited;
static int g_calls;

/* The registered callback: returns the job list that its context names. */
static uint64_t waited(void *ctx)
{
    g_calls++;
    return *(const uint64_t *)ctx;
}

/* A callback that needs no context. */
static uint64_t fixed(void *ctx)
{
    (void)ctx;
    return 0x40044480ull;
}

/* The data of each workload of one instance, as the pass reads it. */
static uint64_t data_of(void *ctx, unsigned wid)
{
    const uint64_t *data = ctx;
    return data[wid];
}

int main(void)
{
    printf(RECORD "on=%d\n", spurs_wait_priority_on());

    /* No callback: no job list. */
    spurs_wait_priority_set(0, 0);
    printf(RECORD "none=%llu\n", (unsigned long long)spurs_wait_priority_job_list());

    /* A registered callback: its answer and its context pass through. */
    g_waited = 0x4004c580ull;
    spurs_wait_priority_set(waited, &g_waited);
    uint64_t list = spurs_wait_priority_job_list();
    printf(RECORD "list=%llx calls=%d\n", (unsigned long long)list, g_calls);
    g_waited = 0;
    printf(RECORD "zero=%llx\n", (unsigned long long)spurs_wait_priority_job_list());

    /* A new registration replaces the old one. A null function removes it. */
    static uint64_t other = 0x40048500ull;
    spurs_wait_priority_set(waited, &other);
    printf(RECORD "replaced=%llx\n", (unsigned long long)spurs_wait_priority_job_list());
    spurs_wait_priority_set(0, &other);
    printf(RECORD "removed=%llx\n", (unsigned long long)spurs_wait_priority_job_list());
    spurs_wait_priority_set(waited, &other);
    spurs_wait_priority_set(0, 0);
    printf(RECORD "removed_without_context=%llx\n", (unsigned long long)spurs_wait_priority_job_list());
    /* A callback without a context is a valid registration. */
    spurs_wait_priority_set(fixed, 0);
    printf(RECORD "null_context=%llx\n", (unsigned long long)spurs_wait_priority_job_list());
    spurs_wait_priority_set(0, 0);

    /* The workload of a job list: the first workload of the instance. */
    uint64_t data[SPURS_WAIT_PRIORITY_WORKLOADS];
    for (unsigned wid = 0; wid < SPURS_WAIT_PRIORITY_WORKLOADS; wid++) data[wid] = 0x1000ull + wid * 0x40ull;
    data[7] = 0;            /* workload 7 is not in use in this instance */
    data[9] = data[3];      /* two workloads with one job list */
    printf(RECORD "found=%d\n", spurs_wait_priority_workload(0x1000ull + 8 * 0x40ull, data_of, data));
    printf(RECORD "other_instance=%d\n", spurs_wait_priority_workload(0, data_of, data));
    printf(RECORD "first_of_two=%d\n", spurs_wait_priority_workload(data[3], data_of, data));
    printf(RECORD "last=%d\n", spurs_wait_priority_workload(data[15], data_of, data));
    printf(RECORD "missing=%d\n", spurs_wait_priority_workload(0x7ffffffffffffull, data_of, data));
    printf(RECORD "no_list=%d\n", spurs_wait_priority_workload(0, data_of, data));
    printf(RECORD "count=%u extra=%u\n", spurs_wait_priority_count(-1), spurs_wait_priority_count(5));

    /* The scan of one free lane: each workload once, the wanted one first,
     * the others in the order of the cursor, 16 steps of the cursor. */
    for (int wanted = -1; wanted < (int)SPURS_WAIT_PRIORITY_WORKLOADS; wanted++) {
        unsigned cursor = 3, seen = 0, count = 0, order = 1, next = 3;
        int first = -1;
        for (unsigned checked = 0; checked < spurs_wait_priority_count(wanted); checked++) {
            int wid = spurs_wait_priority_at(wanted, &cursor, checked);
            if (wid < 0) continue;   /* the position of the wanted workload */
            if (!checked) {
                first = wid;
                if (wanted >= 0) {
                    count++;
                    seen |= 1u << wid;
                    continue;
                }
            }
            count++;
            seen |= 1u << wid;
            while (next == (unsigned)wanted) next = (next + 1) & 15u;
            if ((unsigned)wid != next) order = 0;
            next = (next + 1) & 15u;
        }
        printf(RECORD "scan wanted=%d first=%d count=%u all=%u order=%u cursor=%u\n",
               wanted, first, count, seen == 0xffffu, order, cursor);
    }

    /* The counters and the line: at most one line per second. */
    spurs_wait_priority_report(500);
    spurs_wait_priority_wait();
    spurs_wait_priority_wait();
    spurs_wait_priority_job();
    spurs_wait_priority_report(1500);
    spurs_wait_priority_wait();
    spurs_wait_priority_report(2400);
    spurs_wait_priority_report(2500);
    spurs_wait_priority_report(3500);
    return 0;
}
