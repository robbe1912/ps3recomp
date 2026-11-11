/*
 * ps3recomp - present/flip rate instrumentation (FPS_STATS)
 *
 * Measures the frames a title actually puts on screen: hooks every
 * successful swapchain Present (legacy d3d12 backend render_frame and the
 * live-draw engine's Present / movie Present) plus every accepted guest flip
 * request in cellGcmSys, and reports per second on stderr.
 *
 * Env (each resolved once from getenv, then cached -- GCM_FLIPCOUNT pattern):
 *   FPS_STATS=1         Enable. One report line per elapsed second:
 *                         [FPS] t=5.000s presents=60 flips=60 dt_ms min=15.800 avg=16.666 max=17.900 hist=0/0/0/58/2/0/0/0/0/0
 *                       (t is seconds since the first present; hist buckets
 *                       are frame deltas in ms: <=4 / <=8 / <=12 / <=16 /
 *                       <=20 / <=25 / <=33 / <=50 / <=100 / >100.)
 *                       A one-shot summary prints at process exit.
 *   FPS_STATS_CSV=path  Also write one CSV row per present:
 *                         frame,t_ms,dt_ms,flips_since_prev_present
 *                       dt_ms is 0 on the first row (no predecessor); t_ms is
 *                       ms since the first present. Flushed once per second
 *                       and closed in the exit summary. Summarize with
 *                       recomp/out/debug_feasibility/analyze_fps.py.
 *
 * Hot path: one QueryPerformanceCounter read + integer ops per present;
 * nothing is allocated per frame and the CSV FILE* opens once. Disabled cost
 * is one cached-flag compare.
 */

#ifndef PS3RECOMP_RSX_FPS_STATS_H
#define PS3RECOMP_RSX_FPS_STATS_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 once FPS_STATS is set (and not "0"); resolved on first call. Also
 * registers the exit summary, so a flip-only run (zero presents) still
 * reports. */
int rsx_fps_stats_enabled(void);

/* Timestamp one completed swapchain Present (call AFTER Present returns, so
 * vsync blocking is inside the measured delta). */
void rsx_fps_stats_on_present(void);

/* Count one accepted guest flip request. */
void rsx_fps_stats_on_flip(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_FPS_STATS_H */
