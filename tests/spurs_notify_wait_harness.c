/* Harness for check_spurs_notify_wait.py: the production wait header with
 * authored counter readings, a busy wait timed by its own performance-counter
 * read, and a thread that wakes the timer. */
#include <stdio.h>
#define SPURS_KERNEL_POLL_MS 1
#include "../libs/spurs/spurs_notify_wait.h"

/* Time in 100 ns, read and converted here, not through the header under test. */
static uint64_t own_now(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)((long double)counter.QuadPart * 10000000.0L / (long double)frequency.QuadPart);
}

/* Milliseconds of CPU time; a busy loop, so the system tick plays no part. */
static void busy_milliseconds(double milliseconds)
{
    uint64_t start = own_now();
    while (own_now() - start < (uint64_t)(milliseconds * 10000.0)) { }
}

static void convert(uint64_t counter, uint64_t frequency)
{
    printf("[SPURS_NOTIFY_WAIT] convert counter=%llu frequency=%llu units=%llu\n",
           (unsigned long long)counter, (unsigned long long)frequency,
           (unsigned long long)spurs_kernel_100ns(counter, frequency));
}

/* One wait on the timer; prints the elapsed time and the wait result. */
static void waited(const char* kind, int index, HANDLE timer)
{
    uint64_t start = own_now();
    DWORD result = WaitForSingleObject(timer, 5000);
    uint64_t elapsed = own_now() - start;
    printf("[SPURS_NOTIFY_WAIT] %s index=%d elapsed=%llu wait=%lu\n",
           kind, index, (unsigned long long)elapsed, (unsigned long)result);
}

static HANDLE wake_timer;
static DWORD WINAPI wake_later(void* unused)
{
    (void)unused;
    Sleep(30);
    spurs_poll_wake(wake_timer);
    return 0;
}

int main(void)
{
    /* One second at each common rate, a sub-second remainder, no rate, and a
     * counter whose product with 10000000 exceeds 64 bits. */
    convert(1, 1);
    convert(10000000, 10000000);
    convert(3000000000ull, 3000000000ull);
    convert(12345678901234567ull, 1000000000ull);
    convert(3579500, 3579500);
    convert(0, 0);
    convert((1ull << 62) + 12345, 1ull << 40);
    printf("[SPURS_NOTIFY_WAIT] period ms=%d units=%llu\n", SPURS_KERNEL_POLL_MS,
           (unsigned long long)SPURS_KERNEL_POLL_100NS);

    /* The clock has no system-tick step: two reads around a 2 ms busy wait
     * differ by about 2 ms, also when the wait lies inside one tick. */
    for (int i = 0; i < 8; i++) {
        uint64_t before = spurs_kernel_now();
        busy_milliseconds(2.0);
        uint64_t after = spurs_kernel_now();
        printf("[SPURS_NOTIFY_WAIT] busy index=%d elapsed=%llu after_before=%d\n", i,
               (unsigned long long)(after - before), before <= after);
    }

    HANDLE timer = spurs_poll_timer_create();
    printf("[SPURS_NOTIFY_WAIT] timer created=%d\n", timer != NULL);
    if (!timer) return 1;

    /* The deadline ends the wait: 2 ms, not a system tick. The check reads
     * the shortest sample, so load on the host does not fail it. */
    for (int i = 0; i < 6; i++) {
        spurs_poll_arm(timer, spurs_kernel_now() + 20000);
        waited("deadline", i, timer);
    }

    /* A wake from another thread ends a long wait: 30 ms into a 3 s wait. */
    spurs_poll_arm(timer, spurs_kernel_now() + 30000000);
    wake_timer = timer;
    HANDLE thread = CreateThread(NULL, 0, wake_later, NULL, 0, NULL);
    waited("wake", 0, timer);
    printf("[SPURS_NOTIFY_WAIT] thread created=%d\n", thread != NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    /* A wait that ended does not end the next one: a new arm replaces the
     * pending signal. */
    for (int i = 0; i < 3; i++) {
        spurs_poll_wake(timer);
        waited("signaled", i, timer);
        spurs_poll_arm(timer, spurs_kernel_now() + 20000);
        waited("rearmed", i, timer);
    }
    CloseHandle(timer);
    return 0;
}
