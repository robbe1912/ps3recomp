/*
 * ps3recomp - present/flip rate instrumentation (FPS_STATS). See
 * rsx_fps_stats.h for the env contract and report formats.
 *
 * Cost when FPS_STATS is unset: one cached-flag compare per present/flip.
 * Cost when set: one QueryPerformanceCounter read + integer ops per present;
 * the CSV FILE* opens once, fprintf rows are opt-in, and the per-second
 * report is one stderr flush per second. No per-frame allocation.
 *
 * CSV persistence (FPS_STATS_CSV): rows are formatted into a fixed 64 KiB
 * buffer owned here and written to an unbuffered FILE in one fwrite at
 * whole-row boundaries only -- once per completed 1 s report window, earlier
 * if the next row might not fit -- so no library buffer can write a partial
 * row on its own. (A stdio buffer cannot guarantee that: glibc ignores the
 * size for a setvbuf without a caller buffer and writes 4 KiB blocks
 * mid-row.) After every successful write the [FPS] window line
 * carries csv_persisted=<rows>@<t_ms>: that many complete rows, through that
 * t_ms, were handed to the OS and survive an abrupt owned termination. Rows
 * after the last acknowledged flush (at most one report window of presents)
 * can still be lost by such a termination; nothing is fabricated or repaired.
 * A failed write or flush is reported on stderr with the persisted coverage,
 * and the CSV is closed rather than continued.
 */

#include "rsx_fps_stats.h"

#include <stdio.h>
#include "ps3emu/loadtime_trace.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef RSX_FPS_STATS_TEST_SEAMS
/* Authored clock and stdio faults for scripts/check_fps_csv_persistence.py. */
unsigned long long rsx_fps_stats_test_now_ns(void);
size_t rsx_fps_stats_test_fwrite(const void* data, size_t size, size_t count, FILE* stream);
#define fwrite rsx_fps_stats_test_fwrite
#endif

/* Same clock pattern as cellGcmSys.c get_timestamp_ns. */
static unsigned long long fps_now_ns(void)
{
#ifdef RSX_FPS_STATS_TEST_SEAMS
    return rsx_fps_stats_test_now_ns();
#elif defined(_WIN32)
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER now;
    if (!s_freq.QuadPart)
        QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&now);
    /* Integer tick multiplication wraps before division on long host uptimes.
     * Use the same floating-point conversion as cellGcmSys.c. */
    return (unsigned long long)((double)now.QuadPart * 1000000000.0
                              / (double)s_freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull
         + (unsigned long long)ts.tv_nsec;
#endif
}

#define FPS_HIST_BUCKETS 10
/* Bucket edges in ns. Labels line up by index; the last bucket is open. */
static const unsigned long long s_hist_edges_ns[FPS_HIST_BUCKETS - 1] = {
    4000000ull, 8000000ull, 12000000ull, 16000000ull, 20000000ull,
    25000000ull, 33000000ull, 50000000ull, 100000000ull
};
static const char* const s_hist_labels[FPS_HIST_BUCKETS] = {
    "<=4", "<=8", "<=12", "<=16", "<=20", "<=25", "<=33", "<=50", "<=100", ">100"
};

static int s_enabled = -1;              /* -1 = env not yet resolved */
static unsigned long long s_first_ns;   /* first present; 0 = none yet */
static unsigned long long s_last_ns;    /* most recent present */
static unsigned long long s_total_presents;
static unsigned long long s_all_min, s_all_max, s_all_sum, s_all_deltas;
static unsigned s_total_hist[FPS_HIST_BUCKETS];

/* Current 1 s report window (present thread only). */
static unsigned long long s_win_t0;
static unsigned long long s_win_presents;
static unsigned long long s_win_min, s_win_max, s_win_sum, s_win_deltas;
static unsigned s_win_hist[FPS_HIST_BUCKETS];

/* Guest flip requests. Written by guest PPU threads, read by the present
 * thread and the exit summary: volatile like cellGcmSys.c
 * s_flip_request_count; a race costs one count in one report window, which
 * is below what the report is used for. */
static volatile unsigned long long s_total_flips_v;
static volatile unsigned long long s_win_flips_v;
static volatile unsigned long long s_flips_since_present_v;

static FILE* s_csv;
static unsigned long long s_csv_frame;

/* Rows waiting in s_csv_buf (complete rows only) and persistence accounting. */
#define FPS_CSV_BUFFER_BYTES 65536u
#define FPS_CSV_ROW_MAX_BYTES 128u
static char s_csv_buf[FPS_CSV_BUFFER_BYTES];
static size_t s_csv_pending_bytes;               /* formatted, not yet written */
static unsigned long long s_csv_pending_rows;
static unsigned long long s_csv_persisted_rows;  /* complete rows handed to the OS */
static double s_csv_last_t_ms, s_csv_persisted_t_ms;
static int s_csv_persisted_any;

static void fps_summary(void);

int rsx_fps_stats_enabled(void)
{
    if (s_enabled < 0) {
        const char* e = getenv("FPS_STATS");
        s_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (s_enabled)
            atexit(fps_summary);   /* also reports a flip-only run */
    }
    return s_enabled;
}

/* A write or flush failure is not repaired: report what was persisted and
 * stop recording rather than leave a silently truncated file. */
static void fps_csv_fail(const char* operation, size_t partial_bytes)
{
    fprintf(stderr,
            "[FPS] FPS_STATS_CSV %s failed after %llu rows: persisted %llu"
            " complete rows through t_ms=%.3f",
            operation, s_csv_frame, s_csv_persisted_rows,
            s_csv_persisted_any ? s_csv_persisted_t_ms : 0.0);
    if (partial_bytes)
        fprintf(stderr, ", followed by %llu bytes of an incomplete write",
                (unsigned long long)partial_bytes);
    fprintf(stderr, "; CSV closed, later presents are not recorded\n");
    fclose(s_csv);   /* unbuffered: closing writes nothing further */
    s_csv = NULL;
}

/* Write every complete row formatted so far to the OS. Returns 0 on success. */
static int fps_csv_flush(void)
{
    size_t written;
    if (!s_csv)
        return -1;
    if (s_csv_pending_bytes) {
        written = fwrite(s_csv_buf, 1, s_csv_pending_bytes, s_csv);
        if (written != s_csv_pending_bytes || ferror(s_csv)) {
            fps_csv_fail("write", written);
            return -1;
        }
    }
    s_csv_pending_bytes = 0;
    s_csv_persisted_rows += s_csv_pending_rows;
    s_csv_pending_rows = 0;
    if (s_csv_persisted_rows) {
        s_csv_persisted_t_ms = s_csv_last_t_ms;
        s_csv_persisted_any = 1;
    }
    return 0;
}

/* Format one complete line into the pending buffer. */
static int fps_csv_append(const char* line, int n)
{
    if (n < 0 || (unsigned)n >= FPS_CSV_ROW_MAX_BYTES) {
        fps_csv_fail("format", 0);
        return -1;
    }
    /* Write ahead of a line that might not fit. */
    if (s_csv_pending_bytes + FPS_CSV_ROW_MAX_BYTES > FPS_CSV_BUFFER_BYTES &&
        fps_csv_flush() != 0)
        return -1;
    memcpy(s_csv_buf + s_csv_pending_bytes, line, (size_t)n);
    s_csv_pending_bytes += (size_t)n;
    return 0;
}

static void fps_csv_open(void)
{
    const char* path = getenv("FPS_STATS_CSV");
    int n;
    if (!path || !path[0])
        return;
    s_csv = fopen(path, "w");
    if (!s_csv) {
        fprintf(stderr, "[FPS] FPS_STATS_CSV=%s: fopen failed, CSV disabled\n",
                path);
        return;
    }
    if (setvbuf(s_csv, NULL, _IONBF, 0) != 0) {
        fps_csv_fail("unbuffered setup", 0);
        return;
    }
    {
        char line[FPS_CSV_ROW_MAX_BYTES];
        n = snprintf(line, sizeof line, "frame,t_ms,dt_ms,flips_since_prev_present\n");
        if (fps_csv_append(line, n) == 0)
            fps_csv_flush();   /* the header is durable before the first row */
    }
}

static void fps_csv_row(unsigned long long now, unsigned long long dt)
{
    double t_ms = (double)(now - s_first_ns) / 1e6;
    char line[FPS_CSV_ROW_MAX_BYTES];
    int n = snprintf(line, sizeof line, "%llu,%.3f,%.3f,%llu\n",
                     s_csv_frame, t_ms, (double)dt / 1e6, s_flips_since_present_v);
    if (fps_csv_append(line, n) != 0)
        return;
    s_csv_frame++;
    s_csv_pending_rows++;
    s_csv_last_t_ms = t_ms;
}

static unsigned fps_hist_bucket(unsigned long long dt_ns)
{
    unsigned b = 0;
    while (b < FPS_HIST_BUCKETS - 1 && dt_ns > s_hist_edges_ns[b])
        b++;
    return b;
}

static void fps_flush_window(void)
{
    unsigned b;
    /* Window end in ms since the first present (windows advance in exact
     * 1 s steps, so the tail second prints when it completes). */
    unsigned long long end_ms = s_win_t0 - s_first_ns + 1000000000ull;
    double avg = s_win_deltas
        ? (double)s_win_sum / (double)s_win_deltas / 1e6 : 0.0;

    fprintf(stderr,
            "[FPS] t=%llu.%03llus presents=%llu flips=%llu"
            " dt_ms min=%.3f avg=%.3f max=%.3f hist=",
            end_ms / 1000000000ull, (end_ms / 1000000ull) % 1000ull,
            s_win_presents, s_win_flips_v,
            (double)s_win_min / 1e6, avg, (double)s_win_max / 1e6);
    for (b = 0; b < FPS_HIST_BUCKETS; b++)
        fprintf(stderr, "%s%u", b ? "/" : "", s_win_hist[b]);
    /* Acknowledged persistence boundary for this window (see top). */
    if (s_csv && fps_csv_flush() == 0)
        fprintf(stderr, " csv_persisted=%llu@%.3f", s_csv_persisted_rows,
                s_csv_persisted_any ? s_csv_persisted_t_ms : 0.0);
    fprintf(stderr, "\n");

    s_win_presents = 0;
    s_win_flips_v = 0;
    s_win_min = 0;
    s_win_max = 0;
    s_win_sum = 0;
    s_win_deltas = 0;
    for (b = 0; b < FPS_HIST_BUCKETS; b++)
        s_win_hist[b] = 0;
}

static void fps_summary(void)
{
    unsigned b;
    unsigned long long wall_ns = s_last_ns ? s_last_ns - s_first_ns : 0;
    unsigned long long intervals = s_total_presents ? s_total_presents - 1 : 0;
    double fps = wall_ns
        ? (double)intervals * 1e9 / (double)wall_ns : 0.0;
    double avg = s_all_deltas
        ? (double)s_all_sum / (double)s_all_deltas / 1e6 : 0.0;

    if (!s_enabled)
        return;
    fprintf(stderr,
            "[FPS] summary presents=%llu flips=%llu wall=%.3fs"
            " presents_per_sec=%.2f dt_ms min=%.3f avg=%.3f max=%.3f\n",
            s_total_presents, s_total_flips_v,
            (double)wall_ns / 1e9, fps,
            (double)s_all_min / 1e6, avg, (double)s_all_max / 1e6);
    fprintf(stderr, "[FPS] hist_ms");
    for (b = 0; b < FPS_HIST_BUCKETS; b++)
        fprintf(stderr, " %s:%u", s_hist_labels[b], s_total_hist[b]);
    fprintf(stderr, "\n");
    if (s_csv && fps_csv_flush() == 0) {
        if (fclose(s_csv) != 0) {
            fprintf(stderr, "[FPS] FPS_STATS_CSV close failed after %llu rows\n",
                    s_csv_frame);
        } else {
            fprintf(stderr, "[FPS] csv rows=%llu persisted=%llu through t_ms=%.3f\n",
                    s_csv_frame, s_csv_persisted_rows,
                    s_csv_persisted_any ? s_csv_persisted_t_ms : 0.0);
        }
        s_csv = NULL;
    }
}

void rsx_fps_stats_on_present(void)
{
    unsigned long long dt = 0;
    unsigned long long now;
    unsigned b;

    if (!rsx_fps_stats_enabled())
        return;

    now = fps_now_ns();
    if (!s_first_ns) {
        if (loadtime_on()) loadtime_event("first_present","source=rsx_fps_stats");
        s_first_ns = now;
        s_win_t0 = now;
        fps_csv_open();
    } else {
        dt = now - s_last_ns;
        b = fps_hist_bucket(dt);
        s_win_hist[b]++;
        s_total_hist[b]++;
        if (s_win_deltas == 0) {
            s_win_min = dt;
            s_win_max = dt;
        } else {
            if (dt < s_win_min) s_win_min = dt;
            if (dt > s_win_max) s_win_max = dt;
        }
        s_win_sum += dt;
        s_win_deltas++;
        if (s_all_deltas == 0) {
            s_all_min = dt;
            s_all_max = dt;
        } else {
            if (dt < s_all_min) s_all_min = dt;
            if (dt > s_all_max) s_all_max = dt;
        }
        s_all_sum += dt;
        s_all_deltas++;
    }

    s_last_ns = now;
    s_total_presents++;
    s_win_presents++;

    if (s_csv)
        fps_csv_row(now, dt);
    s_flips_since_present_v = 0;

    /* Aligned windows: a full second prints once per full second; the
     * remainder carries into the next window. */
    while (now - s_win_t0 >= 1000000000ull) {
        fps_flush_window();
        s_win_t0 += 1000000000ull;
    }
}

void rsx_fps_stats_on_flip(void)
{
    if (!rsx_fps_stats_enabled())
        return;
    s_total_flips_v++;
    s_win_flips_v++;
    s_flips_since_present_v++;
}
