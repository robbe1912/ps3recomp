/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "rsx_live_draw.h"
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "rsx_fifo_boundary.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "../../include/ps3emu/guest_call.h" /* g_ps3_guest_caller for direct handler dispatch */
#include "rsx_fps_stats.h"                   /* FPS_STATS=1 flip-rate counter */
#include "rsx_env_once.h"                   /* read-once debug switches on hot paths */

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Precise in-HLE waits.
 *
 * Raw Sleep(1) quantizes to the 15.6 ms scheduler tick whenever Windows
 * treats the process as background: Win10 2004+ ignores timeBeginPeriod for
 * non-foreground processes, and every title launched from a shell script is
 * background. A per-frame Sleep(1) spin then costs 15-31 ms -- MotorStorm's
 * menu paced at 26-31 fps purely from this, with no [WAIT] log line because
 * it never goes through sys_timer_usleep. Waitable timers and events keep
 * their resolution in background processes, so use them for every wait a
 * guest frame can sit in.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static HANDLE gcm_precise_timer(void)
{
    /* Benign init race: a losing thread's handle leaks once; everyone else
     * shares the winner. */
    static HANDLE t;
    HANDLE h = t;
    if (!h) {
        h = CreateWaitableTimerExW(NULL, NULL,
            0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
            TIMER_ALL_ACCESS);
        if (!h) h = CreateWaitableTimerW(NULL, TRUE, NULL);
        t = h;
    }
    return h;
}

static void gcm_sleep1_precise(void)
{
    HANDLE h = gcm_precise_timer();
    if (h) {
        LARGE_INTEGER due; due.QuadPart = -10000;   /* 1 ms, 100 ns units */
        SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(h, 2);
    } else {
        Sleep(1);
    }
}

/* Doorbell for flip completion: the pump (on whatever guest thread crosses an
 * HLE boundary) sets it when a pending flip completes; cellGcmSetWaitFlip
 * sleeps on it and wakes the instant the flip lands instead of polling. */
static INIT_ONCE s_flip_event_once = INIT_ONCE_STATIC_INIT;
static HANDLE s_flip_event;
static BOOL CALLBACK gcm_flip_event_init(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once; (void)parameter; (void)context;
    s_flip_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return s_flip_event != NULL;
}
static HANDLE gcm_flip_done_event(void)
{
    if (!InitOnceExecuteOnce(&s_flip_event_once, gcm_flip_event_init, NULL, NULL)) return NULL;
    return s_flip_event;
}
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static INIT_ONCE s_qpc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK gcm_qpc_init(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once; (void)parameter; (void)context;
    return QueryPerformanceFrequency(&s_qpc_freq);
}
static void ensure_qpc_init(void)
{
    InitOnceExecuteOnce(&s_qpc_once, gcm_qpc_init, NULL, NULL);
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static _Atomic u32 s_flip_status = CELL_GCM_FLIP_STATUS_DONE;

/* FLIP_LAT=1 (diagnostic, read once): where a guest's flip wait goes.
 * Timestamps of the latest edge of each kind; at each cellGcmSetWaitFlip
 * return the wait is split into
 *   to_tick   wait entry -> the vblank beat that completed the flip
 *   to_pump   that beat  -> a guest thread delivering it (ppu_gcm_pump)
 *   to_wake   delivery   -> the waiter running again
 * plus whether the frame drained and presented before that beat, and the
 * length of the frame clock iterations (drain + present) that beats queue
 * behind. Every 64 waits one [FLIPLAT] line; no behaviour change. */
/* All latency timestamps and aggregates are protected by this independent lock.
 * Copy/reset the reporting window under the lock and print after releasing it. */
static SRWLOCK s_flip_lat_lock = SRWLOCK_INIT;
static u64 s_fl_reset, s_fl_marker, s_fl_pstart, s_fl_pend, s_fl_tick, s_fl_pump;
typedef struct {
    u64 n, wait, wait_max, to_tick, to_pump, to_wake, timeouts, presented_first, untimed;
    u64 marker_to_pstart, present, present_n, iter, iter_max, iter_n;
} GcmFlipLatency;
static GcmFlipLatency s_fl;
static int flip_lat_on(void) { return rsx_env_once("FLIP_LAT") != NULL; }
int cellGcm_flip_lat_enabled(void) { return flip_lat_on(); }
static void flip_lat_edge(u64* edge, u64 now)
{
    if (!flip_lat_on()) return;
    AcquireSRWLockExclusive(&s_flip_lat_lock);
    *edge = now;
    ReleaseSRWLockExclusive(&s_flip_lat_lock);
}

/* Prompt flip completion (opt-in; both off by default).
 *
 * A guest blocked in cellGcmSetWaitFlip is released by a flip completion the
 * frame clock publishes on a vblank beat and some guest thread later delivers
 * at an HLE boundary (ppu_gcm_pump). Measured in a MotorStorm race (FLIP_LAT),
 * delivery took 5-7 ms after the beat, and the beat itself came up to one
 * vblank period after the frame had been taken for present.
 *
 * GCM_FLIP_PROMPT=1      a published completion rings the waiter's doorbell
 *                        and the waiter delivers it itself (the same pump an
 *                        HLE boundary runs, on a guest thread).
 * GCM_FLIP_AT_TAKE=1     the frame clock completes the flip as soon as it
 *                        takes the frame that carries it for present (all of
 *                        its commands drained), provided at least one vblank
 *                        period has passed since the last completion; the
 *                        59.94 Hz cap is kept, and the beat remains the
 *                        fallback. Completing after the present instead held
 *                        the title through every present, so one slow present
 *                        (225 ms in a traced race) became a ~340 ms frame;
 *                        the beat never waited for the present either.
 *
 * Generations tie a present to the wait it completes: each new wait
 * (ResetFlipStatus, or a flip request that sets WAITING) opens a generation,
 * a drained flip stamps the generation it belongs to, and taking the frame for
 * present records that stamp. Taking an older frame never completes a newer
 * wait. */
/* Status/generation transitions, completion publication and delivery claims
 * share this blocking lock. Guest callbacks run after releasing it. */
static SRWLOCK s_flip_lock = SRWLOCK_INIT;
static u64 s_flip_wait_gen, s_flip_drained_gen, s_flip_present_gen;
static u64 s_flip_pending_gen, s_flip_last_complete_ns;
static int s_flip_completion_pending;
void ppu_gcm_pump(void);
static int gcm_flip_prompt(void) { return rsx_env_once("GCM_FLIP_PROMPT") != NULL; }
static int gcm_flip_at_take(void) { return rsx_env_once("GCM_FLIP_AT_TAKE") != NULL; }
/* Caller holds s_flip_lock through the corresponding status/frame publication. */
static void gcm_flip_wait_opened(void)
{
    s_flip_wait_gen++;
    s_flip_completion_pending = 0;
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
}
static void gcm_flip_drained(void)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    s_flip_drained_gen = s_flip_wait_gen;
    ReleaseSRWLockExclusive(&s_flip_lock);
}
static int gcm_flip_waiting(u64 generation)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    int waiting = s_flip_wait_gen == generation && s_flip_status == CELL_GCM_FLIP_STATUS_WAITING;
    ReleaseSRWLockExclusive(&s_flip_lock);
    return waiting;
}
static void gcm_flip_ring_waiter(void);

/* Present a drained flip at its marker (with either switch above).
 *
 * The walker already stops at a flip, but the frame clock presented only at
 * get == put. A waiter released before its frame was presented (a beat that
 * lands before the present) could start the next frame first; the drain then
 * ran on into it and one present showed two guest frames -- in a race, 1-3
 * frames of ~2x length per 30 s, each with one flip never shown. So with
 * either switch on, the drain holds at the flip it stopped at until the frame
 * clock has taken that frame, and the frame clock presents a held flip even
 * when put is already past it. Both switches off: unchanged. */
static _Atomic u32 s_flip_hold;             /* drain stopped at an untaken flip */
static int gcm_flip_hold_enabled(void) { return gcm_flip_prompt() || gcm_flip_at_take(); }
/* Present edges from the frame clock: 0 = present starts, 1 = it returned. */
void cellGcm_flip_lat_present(int phase)
{
    if (!flip_lat_on()) return;
    u64 now = get_timestamp_ns();
    AcquireSRWLockExclusive(&s_flip_lat_lock);
    if (phase == 0) {
        s_fl_pstart = now;
        if (s_fl_marker && now >= s_fl_marker) s_fl.marker_to_pstart += now - s_fl_marker;
    } else {
        s_fl_pend = now;
        if (s_fl_pstart && now >= s_fl_pstart) { s_fl.present += now - s_fl_pstart; s_fl.present_n++; }
    }
    ReleaseSRWLockExclusive(&s_flip_lat_lock);
}
/* One frame clock iteration (drain + optional present), in ns. */
void cellGcm_flip_lat_iteration(u64 ns)
{
    if (!flip_lat_on()) return;
    AcquireSRWLockExclusive(&s_flip_lat_lock);
    s_fl.iter += ns; s_fl.iter_n++;
    if (ns > s_fl.iter_max) s_fl.iter_max = ns;
    ReleaseSRWLockExclusive(&s_flip_lat_lock);
}
static void flip_lat_wait_done(u64 enter, u64 leave, unsigned timeouts)
{
    AcquireSRWLockExclusive(&s_flip_lat_lock);
    s_fl.n++;
    u64 wait = leave - enter;
    s_fl.wait += wait; if (wait > s_fl.wait_max) s_fl.wait_max = wait;
    s_fl.timeouts += timeouts;
    const u64 tick = s_fl_tick, pump = s_fl_pump;
    if (tick >= enter && pump >= tick && leave >= pump) {
        s_fl.to_tick += tick - enter; s_fl.to_pump += pump - tick; s_fl.to_wake += leave - pump;
        if (s_fl_pend >= s_fl_reset && s_fl_pend <= tick) s_fl.presented_first++;
    } else {
        s_fl.untimed++;   /* no wait, or completed before entry */
    }
    GcmFlipLatency report = s_fl;
    int print = s_fl.n % 64 == 0;
    if (print) memset(&s_fl, 0, sizeof s_fl);
    ReleaseSRWLockExclusive(&s_flip_lat_lock);
    if (print) {
        const double k = 1e-6;
        const u64 timed = report.n - report.untimed;
        fprintf(stderr, "[FLIPLAT] waits=%llu wait_avg=%.2fms wait_max=%.2fms timed=%llu to_tick=%.2fms to_pump=%.3fms "
                "to_wake=%.3fms presented_before_tick=%llu timeouts=%llu marker_to_present=%.2fms present_avg=%.2fms "
                "iter_avg=%.3fms iter_max=%.2fms iters=%llu%c",
                (unsigned long long)report.n, k * report.wait / report.n, k * report.wait_max, (unsigned long long)timed,
                timed ? k * report.to_tick / timed : 0, timed ? k * report.to_pump / timed : 0,
                timed ? k * report.to_wake / timed : 0, (unsigned long long)report.presented_first,
                (unsigned long long)report.timeouts,
                report.present_n ? k * report.marker_to_pstart / report.present_n : 0,
                report.present_n ? k * report.present / report.present_n : 0,
                report.iter_n ? k * report.iter / report.iter_n : 0, k * report.iter_max,
                (unsigned long long)report.iter_n, 10);
    }
}

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static _Atomic u32 s_flip_request_count = 0;

u32 cellGcm_flip_request_count(void)
{
    return s_flip_request_count;
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;
/* Set by the flip (guest thread, at a get==put frame boundary); consumed by
 * the ticker, which presents the accumulated batch BEFORE draining further --
 * presenting on a raw flip-count change raced the drain and showed empty or
 * mixed batches (wave: black flashes, layout flicker). */
static _Atomic int s_flip_pending = 0;
/* A flip _cellGcmSetFlipCommand queued in the FIFO: | bufferId, bit 8 = ours. */
#define GCM_FLIP_MARKER 0xFEAD0100u
/* ponytail: fixed headroom, far above one frame's commands (Simpsons ~10 KB);
 * derive it from the observed per-frame volume if a title outgrows it. */
#define GCM_FLIP_WRAP_BYTES 0x40000u

int cellGcm_take_flip_pending(void)
{
    return atomic_exchange(&s_flip_pending, 0);
}


/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage
 *
 * set_offset_table/clear_offset_table keep these two host arrays in step with
 * the IO mappings, and that part was always fine. What was not: the tables
 * cellGcmGetOffsetTable hands out are indexed BY THE GUEST --
 * ioAddress[ea >> 20] and eaAddress[io >> 20] -- and it handed over 64-bit host
 * pointers through a struct whose guest form is two 4-byte fields, so the title
 * got two truncated addresses pointing at nothing it could read.
 *
 * The guest-visible copy lives in the reserved GCM window alongside the label
 * and control blocks, published on each call. 4096 pages covers the 4 GiB
 * address space at 1 MiB granularity, 8 KiB a side, and this is not a hot path.
 * 0xFFFF means "not mapped", as it does in the host tables. */
/* Default home for the HLE-visible GCM window; a port overrides it before
 * running guest code when its title claims this address range. */
uint32_t ppu_hle_inject_base = 0x20000000u;

static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

#define GCM_OFFSET_TABLE_PAGES   4096u                 /* 4 GiB / 1 MiB       */
#define GCM_OFFSET_TABLE_IO_ADDR (VM_HLE_INJECT_BASE + 0x3000u)           /* ea >> 20  -> io>>20 */
#define GCM_OFFSET_TABLE_EA_ADDR (VM_HLE_INJECT_BASE + 0x5000u)           /* io >> 20  -> ea>>20 */

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */

/* Handler mask exported for the RSX event delivery path. Bit layout matches
 * the RPCS3 SYS_RSX_EVENT enum (vblank=0x02, flip=0x04, user_cmd=0x80).
 * The runner's vblank tick reads this to decide which events to send to
 * the libgcm interrupt thread. Updated by Set*Handler below.
 *
 * If g_gcm_driver_info_ea is non-zero, the mask is also written to
 * driver_info + 0x12C0 so the game's own libgcm interrupt path can read it
 * directly from guest memory. The runner sets g_gcm_driver_info_ea during
 * RSX context allocation. */
uint32_t g_gcm_handler_mask = 0;
uint32_t g_gcm_driver_info_ea = 0;

static u32 s_flip_handler_opd     = 0;

/* GCM_FLIPCB_ONTICK=1: fire the guest flip handler only when the flip
 * completes (cellGcmTickFlip), not also when it is requested. */
static int gcm_flipcb_on_tick_only(void)
{
    static int v = -1;
    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;
    return v;
}

static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static u32 s_flip_handler_code    = 0;
static u32 s_vblank_handler_code  = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    if (!getenv("YDKJ_HANDLERFIX") || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(_n++<6) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}
static void gcm_update_handler_mask(void)
{
    uint32_t m = 0;
    if (s_vblank_handler_opd) m |= 0x02;
    if (s_flip_handler_opd)   m |= 0x04;
    if (s_user_handler_opd)   m |= 0x80;
    g_gcm_handler_mask = m;
    if (g_gcm_driver_info_ea)
        vm_write32(g_gcm_driver_info_ea + 0x12C0, m);
}

/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Flip timing */
static u64 s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static u32 s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
#ifdef _WIN32
static volatile LONG s_user_command = 0;
#define GCM_USER_STORE(cmd) InterlockedExchange(&s_user_command, (LONG)(cmd))
/* Pair the pending marker with its cause so a later producer cannot
 * overwrite a cause already claimed by the callback pump. */
static volatile LONG64 s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) InterlockedExchange64(&s_user_pending, (LONG64)(value))
#define GCM_USER_PENDING_TAKE() ((u64)InterlockedExchange64(&s_user_pending, 0))
#else
#include <stdatomic.h>
static atomic_uint s_user_command = 0;
#define GCM_USER_STORE(cmd) atomic_store(&s_user_command, (cmd))
static _Atomic(u64) s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) atomic_store(&s_user_pending, (value))
#define GCM_USER_PENDING_TAKE() atomic_exchange(&s_user_pending, 0)
#endif

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  (VM_HLE_INJECT_BASE + 0x0000u)
#define GCM_LABEL_STRIDE      0x10u
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR (VM_HLE_INJECT_BASE + 0x2000u)

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* The guest variable cellGcmInit's ctx_out pointed at -- the title's own
 * gCellGcmCurrentContext. A title that switches to a command buffer of its own
 * does it by repointing this, and until something reads it back the runtime has
 * no way to notice. GCM_CTXDBG=1 reports it. */
static u32 s_gcm_ctx_out_ea = 0;

/* Set by cellGcm_syscall_bringup: this run reaches RSX through the lv2 sys_rsx_*
 * syscalls (libs/video/sys_rsx.c) rather than through the HLE cellGcm* imports. */
static int s_gcm_syscall_mode = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    /* Round UP: a mapping is described by whole 1MB table entries, so a size
     * that is not a multiple of 1MB still needs its final partial page mapped.
     * Truncating dropped that page -- and dropped the mapping ENTIRELY for any
     * size below 1MB (pages == 0) -- after which cellGcmResolveOffset finds no
     * IO entry and falls back to VRAM, where the guest never wrote anything. */
    u32 pages = (size + 0xFFFFFu) >> 20;  /* number of 1MB pages, rounded up */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
    }
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
    }
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Valid tiled pitch sizes (power-of-two aligned, common RSX values) */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_user_handler = NULL;
    s_second_v_handler = NULL;
    s_last_flip_time = get_timestamp_ns();
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    /* Set up the initial IO mapping for the command buffer region */
    if (ioAddress != 0 && ioSize > 0) {
        populate_offset_table(ioAddress, 0, ioSize);

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }

    /* Zero the guest-visible control register (put/get/ref). */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, 0);

    s_gcm_initialized = 1;
    return CELL_OK;
}

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush. The command buffer is the start of the game's IO region. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    if (cmdSize < 0x10000)
        cmdSize = 0x10000;

    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    cellGcmInit(cmdSize, ioSize, ioAddress);

    u32 cmdbuf = ioAddress;                 /* FIFO lives at the IO region start */
    u32 cdata  = galloc ? galloc(16, 16) : 0;
    if (cdata && gwrite32) {
        gwrite32(cdata + 0x0, cmdbuf);              /* begin   */
        gwrite32(cdata + 0x4, cmdbuf + cmdSize);    /* end     */
        gwrite32(cdata + 0x8, cmdbuf);              /* current */
        /* Command-buffer-full callback: a guest OPD {func, toc} routed to
         * hle_gcm_callback -> cellGcm_fifo_recycle. Without a real callback the
         * ring never recycles and the FIFO wedges once `current` reaches `end`. */
        u32 opd = galloc ? galloc(8, 8) : 0;
        if (opd) {
            gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
            gwrite32(opd + 0x4, 0);                              /* toc (unused) */
            gwrite32(cdata + 0xC, opd);                          /* callback OPD */
        } else {
            gwrite32(cdata + 0xC, 0);
        }
        if (ctx_out_addr)
            gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
        s_gcm_ctx_out_ea = ctx_out_addr;   /* the title's gCellGcmCurrentContext */
        s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    }
    return cdata;
}

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_GCM_ERROR_INVALID_VALUE;

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void)
{
    /* Block until the pending flip completes (the present thread marks it done
     * via cellGcmTickFlip, ~once per display refresh). This throttles the title
     * to vsync; without it the guest loops unthrottled and many frames batch
     * into a single host present -> the console text stacks/duplicates.
     *
     * The wait itself must NOT be a Sleep(1) poll: in a background process one
     * sleep is a whole 15.6 ms scheduler tick, and this call sits in the guest
     * render loop every frame (Ice::Render::KickAndDisplayRenderTarget) --
     * MotorStorm's menu lost 15-31 ms per frame here. Sleep on the flip-done
     * doorbell the pump rings; the 33 ms timeout only bounds a missed beat. */
    AcquireSRWLockExclusive(&s_flip_lock);
    const u64 generation = s_flip_wait_gen;
    ReleaseSRWLockExclusive(&s_flip_lock);
    const int lat = flip_lat_on();
    const u64 lat_enter = lat ? get_timestamp_ns() : 0;
    unsigned lat_timeouts = 0;
    const int prompt = gcm_flip_prompt();
#ifdef _WIN32
    HANDLE ev = gcm_flip_done_event();
    for (int i = 0; i < 64 && gcm_flip_waiting(generation); i++) {
        if (prompt) { ppu_gcm_pump(); if (!gcm_flip_waiting(generation)) break; }
        if (ev) { if (WaitForSingleObject(ev, 33) == WAIT_TIMEOUT) lat_timeouts++; }
        else gcm_sleep1_precise();
    }
#else
    /* elsewhere: runtime/platform/win32_compat.h provides Sleep() */
    for (int i = 0; i < 64 && gcm_flip_waiting(generation); i++) {
        if (prompt) { ppu_gcm_pump(); if (!gcm_flip_waiting(generation)) break; }
        Sleep(1);
        lat_timeouts++;
    }
#endif
    if (lat) flip_lat_wait_done(lat_enter, get_timestamp_ns(), lat_timeouts);
    AcquireSRWLockExclusive(&s_flip_lock);
    if (s_flip_wait_gen == generation) s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    ReleaseSRWLockExclusive(&s_flip_lock);
}

/* NID: 0xDF6476BD. The same wait without libgcm's command-buffer space check;
 * this HLE writes no command, so it is the same call. GH3 makes it ~36 times
 * a boot and got a faked CELL_OK (no wait at all) before this was registered. */
void cellGcmSetWaitFlipUnsafe(void) { cellGcmSetWaitFlip(); }

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    { static int n = 0;
      if (rsx_env_once("FLIP_DBG") && n++ < 12)
          fprintf(stderr, "[FLIP] ResetFlipStatus%c", 10); }
    AcquireSRWLockExclusive(&s_flip_lock);
    if (flip_lat_on()) flip_lat_edge(&s_fl_reset, get_timestamp_ns());
    gcm_flip_wait_opened();
    ReleaseSRWLockExclusive(&s_flip_lock);
}

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; cellGcmTickFlip (the ticker's 60Hz vblank
     * beat) completes pending flips. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    { static int n = 0;
      if (rsx_env_once("FLIP_DBG") && n++ < 24)
          fprintf(stderr, "[FLIP] GetFlipStatus -> %u (0=DONE 1=WAITING)%c",
                  s_flip_status, 10); }
    return s_flip_status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip/user handler delivery.
 *
 * Tick producers publish pending work. The runner pumps it at HLE boundaries
 * or on a dedicated callback thread. The pump serializes guest handlers with
 * one another; it does not stop unrelated guest execution on other threads.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
#define GCM_PENDING_SET(bits)  InterlockedOr(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
static volatile LONG s_gcm_pumping = 0;
#define GCM_PUMP_TRY_ENTER() (InterlockedCompareExchange(&s_gcm_pumping, 1, 0) == 0)
#define GCM_PUMP_LEAVE()     InterlockedExchange(&s_gcm_pumping, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  atomic_fetch_or(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
static atomic_flag s_gcm_pumping = ATOMIC_FLAG_INIT;
#define GCM_PUMP_TRY_ENTER() (!atomic_flag_test_and_set_explicit(&s_gcm_pumping, memory_order_acquire))
#define GCM_PUMP_LEAVE()     atomic_flag_clear_explicit(&s_gcm_pumping, memory_order_release)
#endif

/* Caller holds s_flip_lock. A pending bit only wakes the pump; the tagged
 * payload determines which wait may actually be completed. */
static void gcm_flip_queue_locked(void)
{
    s_flip_pending_gen = s_flip_wait_gen;
    s_flip_completion_pending = 1;
    GCM_PENDING_SET(2);
}
static void gcm_flip_queue(void)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    gcm_flip_queue_locked();
    ReleaseSRWLockExclusive(&s_flip_lock);
}
static int gcm_flip_claim(void)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    int current = s_flip_completion_pending && s_flip_pending_gen == s_flip_wait_gen;
    s_flip_completion_pending = 0;
    if (current) s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    ReleaseSRWLockExclusive(&s_flip_lock);
    return current;
}

/* Called by the vblank ticker thread. NO guest code -- advance the vblank count
 * and mark a vblank+flip tick pending for the main thread to deliver. */
static u32 s_fifo_getoff;   /* tentative: defined below */

void cellGcm_request_tick(void)
{
    s_vblank_count++;
    /* GCM_FLIP_NEEDS_FIFO=1: only mark a flip pending once the walker has
     * actually consumed up to `put`.
     *
     * A title's fifo-finish can use flip status as a fast path -- Twisted
     * Metal's reads it first and, if the flip is done, skips waiting for `get`
     * entirely. Completing a flip on the 60 Hz beat regardless of FIFO
     * progress therefore tells it the RSX has caught up when it has not, and
     * it recycles a FIFO block the walker is still inside, overwriting the
     * JUMP out of it. Off by default: a title whose FIFO never drains would
     * stop flipping entirely. */
    { static int need = -1;
      if (need < 0) { const char* e = getenv("GCM_FLIP_NEEDS_FIFO"); need = e ? atoi(e) : 0; }
      if (need) {
          u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
          GCM_PENDING_SET(1);
          if (s_fifo_getoff == put) gcm_flip_queue();
          return;
      } }
    GCM_PENDING_SET(1);
    gcm_flip_queue();
}

/* Deliver pending handlers on the calling host thread. Serialize the entire
 * claim-and-deliver sequence across threads: an HLE boundary and a host ticker
 * may both pump, and newer causes must not overtake already claimed callbacks.
 * A nonblocking guard also prevents nested callbacks from reentering. Leave
 * pending notifications untouched when another pump is active. */
void ppu_gcm_pump(void)
{
    if (!GCM_PUMP_TRY_ENTER()) return;
    long p = (long)GCM_PENDING_TAKE();
    u64 user = GCM_USER_PENDING_TAKE();
    /* GCM_PUMP_DBG=1: how often each guest handler is actually delivered. */
    { static int d = -1; static unsigned nv, nf, calls;
      if (d < 0) d = getenv("GCM_PUMP_DBG") ? 1 : 0;
      if (d) { calls++; if (p & 1) nv++; if (p & 2) nf++;
               if ((calls % 20000) == 0)
                   fprintf(stderr, "[gcm-pump] calls=%u vblank=%u flip=%u vcount=%u%c",
                           calls, nv, nf, s_vblank_count, 10); } }
    if ((p & 1) && s_vblank_handler_opd && g_ps3_guest_caller) {
        ydkj_restore_handler_opd(s_vblank_handler_opd, s_vblank_handler_code);
        g_ps3_guest_caller(s_vblank_handler_opd, (uint64_t)s_vblank_count,
                           0, 0, 0, 0, 0, 0, 0);
    }
    if ((p & 2) && gcm_flip_claim()) {
        if (flip_lat_on()) flip_lat_edge(&s_fl_pump, get_timestamp_ns());
#ifdef _WIN32
        { HANDLE ev = gcm_flip_done_event(); if (ev) SetEvent(ev); }
#endif
        if (s_flip_handler_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(s_flip_handler_opd, s_flip_handler_code);
            g_ps3_guest_caller(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (user) {
        u32 cmd = (u32)user;
        if (s_user_handler_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(s_user_handler_opd, (uint64_t)cmd, 0, 0, 0, 0, 0, 0, 0);
    }
    GCM_PUMP_LEAVE();
}

/* Back-compat: the old direct entry points now just mark a tick pending (so any
 * caller other than the ticker still works) -- delivery stays on the main thread. */
void cellGcmTickVBlank(void) { s_vblank_count++; GCM_PENDING_SET(1); }
/* Only deliver a flip completion when a flip is actually outstanding.
 *
 * Ticking unconditionally at 60 Hz fires the guest's flip handler even when
 * the guest never asked for a flip. A title whose handler touches the flip
 * status then livelocks: Tokyo Jungle's main loop is
 *
 *     do { usleep(100); } while (cellGcmGetFlipStatus() != DONE);
 *     cellGcmResetFlipStatus();
 *
 * and its handler runs code that puts the status back to WAITING, so the
 * poll never saw DONE and the game never rendered another frame. On hardware
 * the callback IS the flip interrupt -- no flip, no callback. */
/* Caller holds s_flip_lock through payload publication. */
static void gcm_flip_publish(u64 now)
{
    flip_lat_edge(&s_fl_tick, now);
    s_flip_last_complete_ns = now;
    gcm_flip_queue_locked();
    if (gcm_flip_prompt()) gcm_flip_ring_waiter();
}

static void gcm_flip_ring_waiter(void)
{
#ifdef _WIN32
    HANDLE ev = gcm_flip_done_event();
    if (ev) SetEvent(ev);
#endif
}

/* GCM_FLIP_AT_TAKE: a beat does not complete a flip that is still waiting to
 * be taken; taking its frame completes it. The frame clock runs its queued
 * beats before its drain and take, so after a slow present the catch-up beats
 * completed a flip requested during that present before its frame was taken.
 * With GCM_FLIP_PROMPT the title started its next frame at once, the drain
 * ran into it, one present showed both frames, and the next take found
 * nothing to present (a race lost 3-4 frames per 30 s this way). A flip left
 * untaken for GCM_FLIP_BEAT_FALLBACK_NS of beats is completed by the beat as
 * before, so a frame that never drains cannot hold the title. Frame-clock
 * thread only: the beat and the take both run there. */
#define GCM_FLIP_BEAT_FALLBACK_NS 1000000000ull
static u64 s_flip_beat_deferred_ns;   /* first beat that left the pending flip to the take */
static int gcm_flip_beat_waits_for_take(u64 now)
{
    if (!gcm_flip_at_take() || !s_flip_pending) return 0;
    if (!s_flip_beat_deferred_ns) s_flip_beat_deferred_ns = now;
    return now - s_flip_beat_deferred_ns < GCM_FLIP_BEAT_FALLBACK_NS;
}

void cellGcmTickFlip(void)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    if (s_flip_status == CELL_GCM_FLIP_STATUS_WAITING) {
        const u64 now = get_timestamp_ns();
        if (!gcm_flip_beat_waits_for_take(now)) {
            s_flip_beat_deferred_ns = 0;
            gcm_flip_publish(now);
        }
    }
    ReleaseSRWLockExclusive(&s_flip_lock);
}

/* GCM_FLIP_AT_TAKE=1: cellGcm_take_flip_pending_synced calls this when the
 * frame clock has taken a frame for present. Returns 1 when it completed the
 * current flip. */
#define GCM_VBLANK_PERIOD_NS 16683333ull   /* 59.94 Hz */
static int gcm_flip_complete_taken_locked(u64 min_interval_ns)
{
    if (!gcm_flip_at_take()) return 0;
    if (s_flip_status != CELL_GCM_FLIP_STATUS_WAITING) return 0;
    if (s_flip_present_gen != s_flip_wait_gen) return 0;   /* an older frame */
    const u64 now = get_timestamp_ns();
    if (now - s_flip_last_complete_ns < min_interval_ns) return 0;   /* keep the vblank cap */
    gcm_flip_publish(now);
    return 1;
}

static int gcm_flip_complete_taken(u64 min_interval_ns)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    int completed = gcm_flip_complete_taken_locked(min_interval_ns);
    ReleaseSRWLockExclusive(&s_flip_lock);
    return completed;
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
static u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* RSX get pointer (IO offset) + one-deep CALL return slot. The FIFO-wrap
 * recycle path teleports these when it resets a ring without a JUMP command. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    return 0;
#endif
}

static u32 s_sema_offset = 0;   /* NV406E semaphore offset (label window) */
/* Keep reserve space for the producer's flip marker and wrap JUMP. */
#define GCM_RECYCLE_SLACK 0x1000u
/* Minimum consumed head room before a producer appends a wrap JUMP. */
#define GCM_RECYCLE_MARGIN 0x20000u
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */
static u32 s_fifo_getoff  = 0;
static u32 s_fifo_calloff = 0;

/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * libgcm binds: sub 1/6 = NV0039 (m2mf buffer copy), sub 2 = NV3062 (context
 * surface 2D), sub 3 = NV309E (swizzled surface), sub 4 = NV308A (image from
 * CPU / inline transfer), sub 5 = NV3089 (scaled image).
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u32 cellGcmResolveLocated(int local, u32 offset);

static struct {
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
} s_gcm2d;

static struct {
    u32 src_dma, dst_surf, fmt, op, clip_pt, clip_sz, out_pt, out_sz;
    u32 ds_dx, dt_dy, in_sz, in_fmt, in_off, in_uv;
} s_nv3089;

/* NV309E "swizzled surface" -- the destination object for a scaled-image blit
 * whose target is a swizzled (Morton-ordered) texture rather than a linear
 * surface. Rubber Ducky binds it to the same subchannel this file previously
 * assumed was always NV308A (image-from-CPU), so its SET_OFFSET was being read
 * as an NV308A "point" and the blit destination was never picked up at all.
 * The two are told apart by method 0x0300: NV308A has no such method. */
static struct {
    u32 fmt;        /* 0x0300: (log2h << 24) | (log2w << 16) | format */
    u32 offset;     /* 0x0304: destination offset                     */
    int active;
    int subch;      /* which subchannel NV309E is bound to. Per-subchannel:
                     * this title has NV309E on 4 and NV308A on 5, and a global
                     * flag made 5's NV308A POINT (also 0x0304) be swallowed as
                     * an NV309E offset, breaking every inline transfer. */
} s_nv309e;

/* NV0039 (NV03_MEMORY_TO_MEMORY_FORMAT) -- buffer copy engine. Register
 * encodings per RPCS3 gcm_enums.h (NV0039_*) and psdevwiki RSXFIFOCommands:
 * 0x2184 CTX_DMA_BUFFER_IN, 0x2188 CTX_DMA_BUFFER_OUT, 0x230C OFFSET_IN,
 * 0x2310 OFFSET_OUT, 0x2314 PITCH_IN, 0x0318 PITCH_OUT, 0x231C
 * LINE_LENGTH_IN, 0x2320 LINE_COUNT, 0x2324 FORMAT, 0x2328 BUFFER_NOTIFY
 * (trigger). The FIFO walker masks methods to 11 bits, so both the full
 * engine address (0x230C & 0x1FFC = 0x030C) and libgcm's short subchannel
 * form arrive here as 0x030C..0x0328.
 *
 * libgcm binds this object for texture/index staging between local and main
 * memory. Those slots alias NV4097 blend/alpha registers (0x30C..0x324) when
 * a title drives 3D on subchannel 1, and alias NV3089's file on 6/7, so the
 * engine only consumes methods once a copy has actually fired (latched
 * 'active'): aliased 3D words never present valid libgcm DMA handles. */
static struct {
    u32 dma_in, dma_out;       /* 0x0184 / 0x0188: 0xFEED0000 local, 0xFEED0001 main */
    u32 offset_in, offset_out; /* 0x030C / 0x0310 */
    u32 pitch_in, pitch_out;   /* 0x0314 / 0x0318 */
    u32 line_length, count;    /* 0x031C / 0x0320 */
    u32 format;                /* 0x0324 */
    int active;                /* a copy has fired on this object */
} s_nv0039;

/* Perform one NV0039 copy. Returns 1 when the accumulated state described a
 * real copy (valid DMA handles, sane geometry, resolvable addresses) and it
 * was executed; 0 otherwise, leaving the caller free to try another decoder. */
static int nv0039_copy(void)
{
    u32 len = s_nv0039.line_length, cnt = s_nv0039.count;
    if (!len || !cnt || len > 0x400000u || cnt > 0x40000u ||
        (u64)len * cnt > 0x1000000ull)   /* >16MB is not a staging copy */
        return 0;
    /* Real NV0039 traffic always carries libgcm DMA handles; aliased NV4097
     * words in this range are blend factors/equations, never 0xFEED000x. */
    if ((s_nv0039.dma_in  != 0xFEED0000u && s_nv0039.dma_in  != 0xFEED0001u) ||
        (s_nv0039.dma_out != 0xFEED0000u && s_nv0039.dma_out != 0xFEED0001u))
        return 0;
    /* Either a straight contiguous run (pitches equal and line-strided), or
     * a pitched per-line copy (both pitches nonzero). */
    int pitched = !(s_nv0039.pitch_in == s_nv0039.pitch_out &&
                    (s_nv0039.pitch_in == len || cnt == 1));
    if (pitched && (!s_nv0039.pitch_in || !s_nv0039.pitch_out))
        return 0;
    int sl = (s_nv0039.dma_in  != 0xFEED0001u);   /* 0xFEED0001 = main mem */
    int dl = (s_nv0039.dma_out != 0xFEED0001u);
    u32 src = cellGcmResolveLocated(sl, s_nv0039.offset_in);
    u32 dst = cellGcmResolveLocated(dl, s_nv0039.offset_out);
    if (src == 0xFFFFFFFFu || dst == 0xFFFFFFFFu)
        return 0;
    { extern u8* vm_base;
      if (!vm_base) return 0;
      const u8* sp = vm_base + src;
      u8*       dp = vm_base + dst;
      if (!pitched) {
          memmove(dp, sp, (size_t)((u64)len * cnt));
      } else {
          for (u32 y = 0; y < cnt; y++)
              memmove(dp + (u64)y * s_nv0039.pitch_out,
                      sp + (u64)y * s_nv0039.pitch_in, len);
      } }
    { static int n = 0;
      if (n++ < 8)
          printf("[NV0039] copy in=0x%X(main=%d) out=0x%X(main=%d) %ux%u "
                 "pitch=%u/%u\n",
                 s_nv0039.offset_in, !sl, s_nv0039.offset_out, !dl,
                 len, cnt, s_nv0039.pitch_in, s_nv0039.pitch_out); }
    return 1;
}

/* Feed one (method, data) pair to the NV0039 state machine. Returns 1 when
 * consumed, 0 when the method belongs to some other decoder. */
static int gcm_nv0039_method(u32 method, u32 data)
{
    switch (method) {
    case 0x0184: s_nv0039.dma_in      = data; break;
    case 0x0188: s_nv0039.dma_out     = data; break;
    case 0x030C: s_nv0039.offset_in   = data; break;
    case 0x0310: s_nv0039.offset_out  = data; break;
    case 0x0314: s_nv0039.pitch_in    = data; break;
    case 0x0318: s_nv0039.pitch_out   = data; break;
    case 0x031C: s_nv0039.line_length = data; break;
    case 0x0320: s_nv0039.count       = data; break;
    case 0x0324: s_nv0039.format      = data; break;
    case 0x0328:   /* BUFFER_NOTIFY: the copy trigger */
        if (nv0039_copy()) { s_nv0039.active = 1; return 1; }
        return s_nv0039.active;
    default:
        return s_nv0039.active;   /* latched: consume follow-on state words */
    }
    return s_nv0039.active;
}

/* Execute one NV3089 blit into the currently-bound NV3062 destination surface.
 * Point-sampled: ds/dx and dt/dy are 20.12 fixed point and are 0x100000 (1.0)
 * for the straight uploads this exists for; a scaled blit still lands, just
 * without filtering. */
/* Morton/Z-order offset for an NV309E swizzled surface: interleave the low
 * min(l2w,l2h) bits of x and y, then append the remaining bits of the larger
 * dimension above them. */
static u32 nv309e_swz(u32 x, u32 y, u32 l2w, u32 l2h)
{
    u32 n = l2w < l2h ? l2w : l2h, off = 0;
    for (u32 i = 0; i < n; i++)
        off |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    if (l2w > l2h)      off |= (x >> n) << (2 * n);
    else if (l2h > l2w) off |= (y >> n) << (2 * n);
    return off;
}

static void nv3089_blit(void)
{
    u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 out_x = s_nv3089.out_pt & 0xFFFF, out_y = s_nv3089.out_pt >> 16;
    u32 in_pitch = s_nv3089.in_fmt & 0xFFFF;
    u32 dst_pitch = s_gcm2d.pitch >> 16;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    /* NV3089_GATE=<file>: log every blit's geometry while <file> exists, so a
     * capture covers the scene of interest instead of the first N since boot. */
    { static const char* gate = (const char*)-1; static unsigned poll; static int open_gate;
      if (gate == (const char*)-1) gate = getenv("NV3089_GATE");
      if (gate) {
          if ((poll++ & 63u) == 0) { FILE* g = fopen(gate, "rb"); open_gate = g != NULL; if (g) fclose(g); }
          if (open_gate) {
              int _sl = (s_nv3089.src_dma != 0xFEED0001u), _dl = (s_gcm2d.dst_dma != 0xFEED0001u);
              printf("[NV3089G] fmt=0x%X out=%ux%u at %u,%u in=%ux%u uv=0x%08X ds=0x%X dt=0x%X src=0x%08X/%u dst=0x%08X/%u swz=%d%c",
                     s_nv3089.fmt & 0xFF, out_w, out_h, out_x, out_y,
                     s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, s_nv3089.in_uv,
                     s_nv3089.ds_dx, s_nv3089.dt_dy,
                     cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch,
                     cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch,
                     s_nv309e.active, 10);
          }
      } }
    /* A copy out of a surface the live renderer drew exists only on the GPU;
     * mirror 1:1 blits there too (the guest-memory copy below still runs). */
    if (s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        rsx_live_draw_enabled()) {
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int sl = (s_nv3089.src_dma != 0xFEED0001u);
        const int dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
        rsx_live_draw_blit(sl ? 0u : 1u, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                           dl ? 0u : 1u, s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                         + (s_nv3089.out_pt & 0xFFFF) * 4u, dst_pitch,
                           out_w, out_h);
    } else if (rsx_live_draw_enabled()) {
        /* Scaled: a resolve. Copy it on the GPU into the display buffer; fall
         * back to presenting the source when that cannot be done. */
        const u32 sl = s_nv3089.src_dma != 0xFEED0001u ? 0u : 1u;
        const u32 dl = s_gcm2d.dst_dma  != 0xFEED0001u ? 0u : 1u;
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int rok = rsx_live_draw_resolve_blit(sl, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                                        s_nv3089.ds_dx, s_nv3089.dt_dy, dl, s_gcm2d.dst_offset,
                                        dst_pitch, out_x, out_y, out_w, out_h);
        if (!rok && s_nv3089.in_uv <= 0x00100010u && !s_nv3089.out_pt)
            rsx_live_draw_note_resolve(sl, s_nv3089.in_off, dl, s_gcm2d.dst_offset);
    }
    /* Only the 32-bit colour formats are handled; anything else would need a
     * per-format converter and is better skipped loudly than written wrong. */
    u32 f = s_nv3089.fmt & 0xFF;
    /* NOTE: format 0x3 appears here as a downsample chain (1024x512 -> 1024x256
     * -> 512x128 -> 256x64 -> 128x32, all into one destination) that builds a
     * mip/blur pyramid, and skipping it leaves that texture empty. Treating it
     * as 4-byte A8R8G8B8 like 7/8/0xD is WRONG -- it floods the scene blue
     * (flat-blue coverage 17k -> 537k pixels), so its pixel layout differs.
     * Decode it properly before enabling; do not just add it to this list. */
    /* NV309E swizzled destination. Colour format 0x3 is A8R8G8B8 in the
     * scaled-image enum; this title uses it to build a mip/blur pyramid into a
     * swizzled texture (1024x512 -> 1024x256 -> ... all through NV309E).
     * Writing it LINEARLY into the NV3062 destination is what flooded the scene
     * blue -- wrong destination and wrong layout. */
    { u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
      static int en = -1;
      /* On by default: this is the mip/blur pyramid. Without it textures have
       * no mip levels, which shows up as aliased blocky tiles and a noise patch
       * where a minified surface should be, and the shell that samples the
       * pyramid draws untextured. NV309E_BLIT=0 disables. */
      if (en < 0) { const char* e = getenv("NV309E_BLIT"); en = e ? atoi(e) : 1; }
      if (en && f == 3 && s_nv309e.active && s_nv309e.offset &&
          (1u << l2w) == out_w && (1u << l2h) == out_h) {
        int sl = (s_nv3089.src_dma != 0xFEED0001u);
        u32 src2 = cellGcmResolveLocated(sl, s_nv3089.in_off);
        u32 dst2 = cellGcmResolveLocated(1, s_nv309e.offset);
        u32 u0b = s_nv3089.in_uv & 0xFFFF, v0b = s_nv3089.in_uv >> 16;
        /* Copy words verbatim through raw pointers rather than vm_read32/
         * vm_write32: this moves ~500k pixels per blit and several blits run per
         * frame, and the two byte swaps would cancel anyway. */
        { extern u8* vm_base;
          const u8* sbase = vm_base + src2;
          u8* dbase = vm_base + dst2;
          for (u32 y = 0; y < out_h; y++) {
              u64 sv = ((u64)v0b << 8) + (u64)y * s_nv3089.dt_dy;
              const u8* srow = sbase + (u32)(sv >> 20) * in_pitch;
              for (u32 x = 0; x < out_w; x++) {
                  u64 su = ((u64)u0b << 8) + (u64)x * s_nv3089.ds_dx;
                  memcpy(dbase + (u64)nv309e_swz(x, y, l2w, l2h) * 4,
                         srow + (u32)(su >> 20) * 4, 4);
              }
          } }
        { static int _n = 0; static int cap = -1;
          if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && _n < cap) { _n++;
            printf("[NV3089] swizzled %ux%u fmt=0x%X src=0x%08X -> NV309E dst=0x%08X%c",
                   out_w, out_h, f, src2, dst2, 10); } }
        return;
      } }
    /* 0x3 is A8R8G8B8 in the scaled-image format enum. This title uses it for
     * two different things: the swizzled mip/blur pyramid handled above, and
     * (with a LINEAR NV3062 destination) copies of the rendered scene surface
     * into the textures its water and effects sample -- e.g.
     * src=0xCE340000 (the scene) -> dst=0xC3746200, which is the single most
     * sampled blended texture in the frame. Skipping those left every one of
     * those textures empty. Same 4-byte pixel layout as the formats already
     * handled. */
    if (f != 3 /* A8R8G8B8 (scale-format enum) */ &&
        f != 7 /* A8R8G8B8 */ && f != 8 /* X8R8G8B8 */ && f != 0xD /* A8B8G8R8 */) {
        /* Log the GEOMETRY of a skipped blit, not just the format: a full-screen
         * copy from the scene surface to a registered display buffer is the
         * composite this title needs, and is worth telling apart from some
         * incidental small transfer. */
        { int _sl = (s_nv3089.src_dma != 0xFEED0001u);
          int _dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
          printf("[NV3089] SKIPPED fmt=0x%X  %ux%u  src=0x%08X(pitch %u,%s) -> dst=0x%08X(pitch %u,%s) at %u,%u%c",
                 f, out_w, out_h,
                 cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch, _sl?"LOCAL":"MAIN",
                 cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch, _dl?"LOCAL":"MAIN",
                 out_x, out_y, 10); }
        static int _w = 0;
        if (_w++ < 4) printf("[NV3089] unsupported colour format 0x%X, blit skipped\n", f);
        return;
    }
    int src_local = (s_nv3089.src_dma != 0xFEED0001u);
    int dst_local = (s_gcm2d.dst_dma  != 0xFEED0001u);
    u32 src = cellGcmResolveLocated(src_local, s_nv3089.in_off);
    u32 dst = cellGcmResolveLocated(dst_local, s_gcm2d.dst_offset);
    u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 start */
    /* Clip to SET_CLIP_POINT/SIZE (destination coordinates). A title tiling a
     * resolve in 512x512 blocks relies on it to stop at the surface edge:
     * Tornado Outbreak's bottom band is 512 rows tall with 208 left in the
     * display buffer, and writing all 512 ran on into the VRAM after it --
     * where its full-screen quad's vertices were staged, so the quad drew with
     * all-zero positions. A zero clip size means unclipped. */
    u32 cx0 = s_nv3089.clip_pt & 0xFFFF, cy0 = s_nv3089.clip_pt >> 16;
    u32 cx1 = (s_nv3089.clip_sz & 0xFFFF) ? cx0 + (s_nv3089.clip_sz & 0xFFFF) : 0xFFFFFFFFu;
    u32 cy1 = (s_nv3089.clip_sz >> 16)    ? cy0 + (s_nv3089.clip_sz >> 16)    : 0xFFFFFFFFu;
    /* Bytes per pixel from the SDK enums (CELL_GCM_TRANSFER_SCALE_FORMAT_*:
     * 7 = R5G6B5, 8 = Y8 -- not 32-bit as they were once labelled). When the
     * destination surface format agrees it is a plain copy of that width; a
     * real format conversion keeps the old 4-byte copy rather than guess. */
    u32 sbpp = (f == 7) ? 2u : (f == 8) ? 1u : 4u;
    u32 dbpp = (s_gcm2d.color_fmt == 4u /* R5G6B5 */) ? 2u
             : (s_gcm2d.color_fmt == 1u /* Y8 */)     ? 1u : 4u;
    u32 bpp = (sbpp == dbpp) ? sbpp : 4u;
    for (u32 y = 0; y < out_h; y++) {
        u32 dy = out_y + y;
        if (dy < cy0 || dy >= cy1) continue;
        u64 sv = ((u64)v0 << 8) + (u64)y * s_nv3089.dt_dy;         /* 20.12 */
        u32 sy = (u32)(sv >> 20);
        for (u32 x = 0; x < out_w; x++) {
            u32 dx = out_x + x;
            if (dx < cx0 || dx >= cx1) continue;
            u64 su = ((u64)u0 << 8) + (u64)x * s_nv3089.ds_dx;
            u32 sx = (u32)(su >> 20);
            u32 s = src + sy * in_pitch + sx * bpp;
            u32 d = dst + dy * dst_pitch + dx * bpp;
            if (bpp == 4)      vm_write32(d, vm_read32(s));
            else if (bpp == 2) vm_write16(d, vm_read16(s));
            else               vm_write8(d, vm_read8(s));
        }
    }
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV3089] %ux%u fmt=0x%X src=0x%08X(pitch %u) -> dst=0x%08X(pitch %u) at %u,%u\n",
               out_w, out_h, f, src, in_pitch, dst, dst_pitch, out_x, out_y); } }
}

static void gcm_2d_method(u32 subch, u32 method, u32 data)
{
    /* GCM2D_TRACE=1: histogram of (subchannel, method) actually seen. The
     * subchannel a title binds each 2D object to is libgcm-version-specific and
     * SET_OBJECT binds are not tracked, so state landing on an unexpected
     * subchannel is silently dropped -- which looks like a blit using stale
     * destination state rather than like missing state. */
    { static int tr = -1;
      if (tr < 0) { const char* e = getenv("GCM2D_TRACE"); tr = e ? atoi(e) : 0; }
      if (tr) {
          static u8 seen[8][1024]; static u64 n = 0;
          u32 mi = (method >> 2) & 1023;
          if (subch < 8 && !seen[subch][mi]) {
              seen[subch][mi] = 1;
              printf("[GCM2D-TRACE] subch=%u method=0x%04X (first, data=0x%08X)%c",
                     subch, method, data, 10);
          }
          if (++n == 400000ull) {
              printf("[GCM2D-TRACE] --- subchannels used: ");
              for (u32 sc = 0; sc < 8; sc++) {
                  u32 c = 0; for (u32 m = 0; m < 1024; m++) c += seen[sc][m];
                  if (c) printf("%u(%u methods) ", sc, c);
              }
              printf("---%c", 10);
          }
      } }
    /* Subchannel bindings are libgcm-version-specific (SET_OBJECT binds are
     * not tracked): demosaic's SDK emits dest-surface on sub 3 and image-from-
     * CPU on sub 5 (cellGcmSetInlineTransfer: 0x4630C / 0xCA304 / 0xA400
     * headers); other versions use the classic 2 / 4. Accept both. */
    if (subch == 2 || subch == 3) {         /* NV3062 context surface 2D */
        switch (method) {
        case 0x0184: case 0x0188: s_gcm2d.dst_dma = data; return;
        case 0x0300: s_gcm2d.color_fmt  = data; return;
        case 0x0304: s_gcm2d.pitch      = data; return;
        case 0x030C: s_gcm2d.dst_offset = data; return;
        }
        return;
    }
    if (subch == 4 || subch == 5) {         /* NV308A image-from-CPU, or NV309E */
        /* NV309E swizzled-surface state. SET_FORMAT (0x0300) does not exist on
         * NV308A, so seeing it identifies the object bound here. */
        if (method == 0x0300) { s_nv309e.fmt = data; s_nv309e.active = 1;
                                s_nv309e.subch = (int)subch;
            { static int _n = 0; if (_n++ < 4)
                printf("[NV309E] format=0x%08X (log2 %ux%u fmt=0x%X)%c", data,
                       1u << ((data >> 16) & 0xFF), 1u << ((data >> 24) & 0xFF),
                       data & 0xFFFF, 10); }
            return; }
        if (method == 0x0304 && s_nv309e.active && s_nv309e.subch == (int)subch) {
            s_nv309e.offset = data; return; }
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return;
        case 0x0308: s_gcm2d.size_out = data; return;
        case 0x030C: return;                /* SIZE_IN */
        }
        if (method >= 0x0400 && method <= 0x07FC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers use format Y32 (4 bytes/px, one row per point.y).
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base = cellGcmResolveLocated(local, s_gcm2d.dst_offset);
            { static int _it = 0;
              if (_it++ < 6)
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u) = 0x%08X\n",
                         base + py * dst_pitch + px * 4, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, data); }
            vm_write32(base + py * dst_pitch + px * 4, data);
            return;
        }
        return;
    }
    /* NV3089 "scaled image from memory" -- the blit PSGL uses to move a texture
     * it has assembled elsewhere into the VRAM the sampler reads. Without it a
     * title's textures stay whatever the destination was before (zero), so
     * geometry renders flat no matter how correct the sampler is.
     *
     * Registers: 0x0184 source DMA, 0x0198 destination surface (the NV3062
     * state above), 0x0300 colour format, 0x0304 operation, 0x0308/0x030C clip
     * point/size, 0x0310/0x0314 out point/size, 0x0318/0x031C ds/dx dt/dy in
     * 20.12 fixed point, 0x0400 in size, 0x0404 (origin<<16)|pitch, 0x0408 in
     * offset, 0x040C in u/v start -- which is also the trigger. */
    if (subch == 6 || subch == 7) {
        /* Some SDK revisions bind NV0039 on the subchannel this file gives
         * to NV3089. The two register files alias, but the triggers differ
         * (0x0328 vs 0x040C) and NV0039 only latches on after a real copy,
         * so both engines can share the subchannel. */
        if (gcm_nv0039_method(method, data)) return;
        switch (method) {
        case 0x0184: s_nv3089.src_dma   = data; return;
        case 0x0198: s_nv3089.dst_surf  = data; return;
        case 0x0300: s_nv3089.fmt       = data; return;
        case 0x0304: s_nv3089.op        = data; return;
        case 0x0308: s_nv3089.clip_pt   = data; return;
        case 0x030C: s_nv3089.clip_sz   = data; return;
        case 0x0310: s_nv3089.out_pt    = data; return;
        case 0x0314: s_nv3089.out_sz    = data; return;
        case 0x0318: s_nv3089.ds_dx     = data; return;
        case 0x031C: s_nv3089.dt_dy     = data; return;
        case 0x0400: s_nv3089.in_sz     = data; return;
        case 0x0404: s_nv3089.in_fmt    = data; return;
        case 0x0408: s_nv3089.in_off    = data; return;
        case 0x040C: s_nv3089.in_uv     = data; nv3089_blit(); return;
        }
        return;
    }
    /* Anything else on 2..7 is an unhandled 2D-engine method (NV0039 is
     * handled above and via the subchannel-1 walker hook). */
    /* GCM2D_DBG=<N> raises the cap and adds the data word: the fixed 8 showed
     * only that SOMETHING was unhandled, not enough to implement it. */
    static int warned = 0, wcap = -1;
    if (wcap < 0) { const char* e = getenv("GCM2D_DBG"); wcap = e ? atoi(e) : 8; }
    if (warned < wcap) { warned++;
        printf("[GCM2D-UNH] subch %u method 0x%04X data=0x%08X\n", subch, method, data); }
    if (0)
        printf("[cellGcmSys] FIFO subch %u method 0x%04X (unhandled 2D engine)\n",
               subch, method);
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    if (!s_flip_pending) return 0;
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put && !(gcm_flip_hold_enabled() && s_flip_hold)) return 0;   /* frame not fully drained yet */
    AcquireSRWLockExclusive(&s_flip_lock);
    if (!atomic_exchange(&s_flip_pending, 0)) {
        ReleaseSRWLockExclusive(&s_flip_lock);
        return 0;
    }
    s_flip_hold = 0;
    s_flip_beat_deferred_ns = 0;
    s_flip_present_gen = s_flip_drained_gen;
    ReleaseSRWLockExclusive(&s_flip_lock);
    cellGcm_flip_lat_present(0);
    gcm_flip_complete_taken(GCM_VBLANK_PERIOD_NS);
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

/* A JUMP/CALL whose target has no IO mapping must not be taken. Taking one
 * teleports the walker into unmapped space, where it can never reach `put`
 * again: the FIFO silently stops being processed, no fences are published, and
 * a title spinning on ctrl->ref waits forever with no indication why. Tokyo
 * Jungle did exactly this -- get left the 1 MB ring for IO 0x00F00000, which it
 * never mapped, while put sat at 0x604. Stop at the bad word and say so. */
static void gcm_fifo_bad_branch(const char* kind, u32 target, u32 word)
{
    static int n = 0;
    if (n++ < 8)
        fprintf(stderr, "[cellGcmSys] FIFO %s to unmapped IO 0x%08X (word 0x%08X) "
                "-- not taken, drain stops here\n", kind, target, word);
}

/* A bad branch nearly always means the walker mis-counted a method batch a few
 * dwords back and is now reading vertex data -- a float 1.0f is 0x3F800000,
 * which decodes as JUMP 0x1F800000. Dump the run-up: the command that consumed
 * the wrong count is in here. */
/* Last words the walker actually consumed, so a bad branch can show the decode
 * chain that led there rather than raw memory around it. */
#define GCM_TRACE_N 64
static u32 s_tr_off[GCM_TRACE_N], s_tr_w[GCM_TRACE_N];
static u32 s_tr_i = 0;


static void gcm_fifo_dump_around(u32 getoff)
{
    static int d = 0;
    if (!rsx_env_once("GCM_RECDBG") || d++ >= 3) return;
    for (u32 t = 0; t < GCM_TRACE_N; t++) {
        u32 k = (s_tr_i + t) % GCM_TRACE_N;
        if (!s_tr_w[k]) continue;
        u32 w = s_tr_w[k];
        fprintf(stderr, "[FIFOSTEP] io=%08X w=%08X  type=%u method=0x%04X count=%u\n",
                s_tr_off[k], w, w >> 29, w & 0x1FFCu, (w >> 18) & 0x7FFu);
    }
    for (int k = -20; k <= 2; k++) {
        u32 io = getoff + (u32)(k * 4);
        u32 ea = gcm_io2ea(io);
        if (!ea) continue;
        fprintf(stderr, "[FIFODUMP] %+3d io=%08X w=%08X%s\n",
                k, io, vm_read32(ea), k ? "" : "   <-- bad word");
    }
}


static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        static int _o = 0;
        if (_o++ < 4) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    s_ref_qtail = t + 1;
}

/* Global fence-publication counter: lets the lwmutex convoy trace correlate a
 * long lock hold with the number of paced fence publications inside it (the
 * 200us pacing x hundreds of one-ahead fences = the ~156ms holds). */
volatile long long g_gcm_ref_pub_count = 0;
/* Absolute microseconds from the QPC hardware counter -- the SAME origin in every
 * translation unit, so two probes in different files can be compared directly.
 * That is the whole point: every wrong conclusion about this stall came from
 * comparing orderings instead of times. */
unsigned long long ps3_qpc_us(void)
{
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (unsigned long long)(n.QuadPart / (f.QuadPart / 1000000));
}

static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    /* stderr, deliberately: [DRAIN] uses printf and stdout is block-buffered
     * when redirected, so its interleaving with an stderr probe is not a
     * timeline. Same stream = same ordering. */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_REFPUB") ? 1 : 0;
      if (_rd) { static unsigned long _n = 0; if (++_n <= 24)
        fprintf(stderr, "[refpub] t=%lluus #%lu wrote 0x%08X -> readback 0x%08X\n", ps3_qpc_us(), _n,
                s_ref_q[h % GCM_REF_QLEN], vm_read32(GCM_CONTROL_GUEST_ADDR + 8)); } }
    s_ref_qhead = h + 1;
    g_gcm_ref_pub_count++;
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static void gcm_rsx_process_fifo_unlocked(void);

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void*)s_gcm_kick_ev;
}

/* The present thread's idle wait. It used to be a flat Sleep(4), which meant
 * the kick above was created, signalled -- and waited on by nobody, so a
 * starved guest still paid the full tick. Waiting here instead costs nothing
 * when no one kicks (the timeout is the old sleep) and drains immediately when
 * someone does. */
void cellGcm_fifo_kick_wait(unsigned ms)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_KICK"); on = (e && *e == '0') ? 0 : 1; }
    HANDLE ev = on ? (HANDLE)cellGcm_fifo_kick_event() : NULL;
    if (ev) WaitForSingleObject(ev, ms);
    else    Sleep(ms);
}


/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    static LARGE_INTEGER s_freq;
    static volatile LONGLONG s_last_pub;
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock))
        return;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG min_gap = s_freq.QuadPart / 5000;          /* ~200us */
    int dry = 0;
    if (now.QuadPart - s_last_pub >= min_gap) {
        gcm_ref_publish_one();
        s_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
}

void cellGcm_rsx_process_fifo(void)
{
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_rsx_process_fifo_unlocked();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
}

int cellGcm_with_fifo_boundary(CellGcmFifoBoundaryHandler handler, void *opaque)
{
    if (!handler) return 0;
    int result = 0;
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    if (s_gcm_context_ea || s_gcm_syscall_mode) {
        gcm_rsx_process_fifo_unlocked();
        u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
        atomic_thread_fence(memory_order_acquire);
        u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
        if (get == put) result = handler(opaque);
    }
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    return result;
}

static void gcm_rsx_process_fifo_unlocked(void)
{
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      if (rsx_env_once("GCM_RATE")) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    static rsx_state s_state;
    static int  s_inited = 0;
    extern u32 g_rsx_last_reference;

    /* A statically-linked libgcm (PS3 firmware modules -- ps1_netemu) never calls
     * cellGcmSetupContext, so there is no CellGcmContextData EA to gate on: it gets
     * its ring through sys_rsx_context_allocate instead. The walk below only needs
     * `put` and the IO table, both of which the syscall layer fills; the one part
     * that does read the context (the ring-recycle heuristic) is separately guarded
     * by s_gcm_ctx_out_ea, which stays 0 on that path. */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    /* Present the flip this drain stopped at before decoding the frame
     * queued behind it (GCM_FLIP_PROMPT / GCM_FLIP_AT_TAKE). */
    if (s_flip_hold && !s_flip_pending) s_flip_hold = 0;   /* that flip was not accepted */
    if (s_flip_hold && s_flip_pending && gcm_flip_hold_enabled()) return;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }

    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    /* The words behind `put` were stored by another thread (the title, or the
     * recycle below) before it stored `put`. On x86 the hardware keeps loads
     * in order; on arm64 it does not, so without an acquire here the ring
     * word can be fetched before `put` was and come back stale -- the walker
     * then runs off the end of the previous frame's bytes until it meets a
     * word that is not a command, and parks there forever. */
    atomic_thread_fence(memory_order_acquire);

    /* GCM_CTXDBG=1: which context the title is actually driving. Ours is written
     * once at init; if the title repoints gCellGcmCurrentContext at a buffer of
     * its own, every field below moves with it and the FIFO the runtime walks
     * stops being the one the title fills. Reported on change, not per tick. */
    if (rsx_env_once("GCM_CTXDBG") && s_gcm_ctx_out_ea) {
        static u32 last_ptr = 0xFFFFFFFFu, last_cur = 0xFFFFFFFFu;
        u32 ptr = vm_read32(s_gcm_ctx_out_ea);
        u32 cur = ptr ? vm_read32(ptr + 0x8) : 0;
        if (ptr != last_ptr || cur != last_cur) {
            last_ptr = ptr; last_cur = cur;
            fprintf(stderr, "[CTX] gCellGcmCurrentContext@0x%08X -> 0x%08X  "
                    "begin=%08X end=%08X current=%08X callback=%08X  (ours=0x%08X)\n",
                    s_gcm_ctx_out_ea, ptr,
                    ptr ? vm_read32(ptr + 0x0) : 0, ptr ? vm_read32(ptr + 0x4) : 0,
                    cur, ptr ? vm_read32(ptr + 0xC) : 0, s_gcm_context_ea);
            fflush(stderr);
        }
    }

    u32 start_off = s_fifo_getoff;
    if (rsx_env_once("GCM_DRAINDBG")) {
        static int n = 0;
        if (n++ % 60 == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    /* GCM_SCANREF=1 (one-shot): list every NV406E_SET_REFERENCE actually
     * present in 0..put. "The title waits for a fence we never published" has
     * two very different causes -- we missed the command, or it was never
     * submitted -- and only a direct scan of the ring separates them. */
    { static int scanned = 0;
      if (!scanned && rsx_env_once("GCM_SCANREF") && put >= 0x19F0) { scanned = 1;
        unsigned found = 0;
        for (u32 io = 0; io + 8 <= put; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w & 0x1FFCu) == 0x0050u && (w >> 29) == 0) {
                u32 dea = gcm_io2ea(io + 4);
                fprintf(stderr, "[SCANREF] io=%08X w=%08X count=%u arg=%08X\n",
                        io, w, (w >> 18) & 0x7FFu, dea ? vm_read32(dea) : 0xDEADu);
                found++;
            }
        }
        fprintf(stderr, "[SCANREF] %u SET_REFERENCE in 0..%08X\n", found, put);
      } }

    /* GCM_FIFO_SNAP=N: dump N snapshots of the raw words at both ends of the
     * ring -- what `get` is about to decode, and what the title just wrote at
     * `put`. When get and put drift apart, the two windows say immediately
     * whether get is grinding through NOPs/garbage or the title is writing
     * somewhere get can never reach. The [FIFOSTEP] trace only ever fired on a
     * bad branch, which is exactly the case where the drift is silent. */
    { static int snap = -1;
      if (snap < 0) { const char* e = getenv("GCM_FIFO_SNAP"); snap = e ? atoi(e) : 0; }
      if (snap > 0 && put) {
          snap--;
          fprintf(stderr, "[SNAP] get=%08X put=%08X\n", s_fifo_getoff, put);
          for (int k = 0; k < 16; k++) {
              u32 gio = s_fifo_getoff + (u32)(k * 4), pio = put + (u32)(k * 4);
              u32 gea = gcm_io2ea(gio), pea = gcm_io2ea(pio);
              fprintf(stderr, "[SNAP]  get+%-3d io=%08X w=%08X   put+%-3d io=%08X w=%08X\n",
                      k * 4, gio, gea ? vm_read32(gea) : 0xDEADDEADu,
                      k * 4, pio, pea ? vm_read32(pea) : 0xDEADDEADu);
          }
          fflush(stderr);
      } }

    int budget = 0x100000;                    /* words per tick cap */
    /* GCM_DRAINDBG also reports WHY each pass stopped. A drain that ends far
     * short of `put` every tick is the signature of a FIFO that can never
     * catch up, and the reason is the whole diagnosis. */
    const char* why = "caught-up";
    while (s_fifo_getoff != put && budget-- > 0) {
        u32 ea = gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            /* An absent mapping does not authorize a new entry point.
             * Wait for mapping repair or an explicit driver FIFO reset. */
            static u32 last_bad; static int n;
            if (s_fifo_getoff != last_bad || n < 4) {
                last_bad = s_fifo_getoff;
                if (n++ < 16)
                    fprintf(stderr, "[cellGcmSys] FIFO get=0x%08X has no IO "
                            "mapping -- drain stops, put=0x%08X\n",
                            s_fifo_getoff, put);
            }
            why = "no-io-mapping";
            break;
        }
        u32 w = vm_read32(ea);
        s_tr_off[s_tr_i % GCM_TRACE_N] = s_fifo_getoff;
        s_tr_w[s_tr_i % GCM_TRACE_N] = w;
        s_tr_i++;
        u32 type = w >> 29;

        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            extern s32 cellGcmSetFlipCommand(u32 bufferId);
            s_fifo_getoff += 4;
            /* Count the flip WORDS the title actually writes into the FIFO,
             * separately from the presents that result. When a title stops
             * appearing on screen the first question is which side stopped:
             * this counter still climbing while presents do not is a runtime
             * problem, both stopping together is the guest's. */
            { static unsigned long long flips = 0; static int dbg = -1;
              if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
              ++flips;
              if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
                  fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                          flips, 10); }
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
                /* Queued by _cellGcmSetFlipCommand, which already did the
                 * guest-visible half of the request. */
                s_current_display_buffer_id = w & 7u;
                if (flip_lat_on()) flip_lat_edge(&s_fl_marker, get_timestamp_ns());
                gcm_flip_drained();
                s_flip_pending = 1;
                s_flip_request_count++;
                /* FPS_STATS=1: statically-linked libgcm (MotorStorm) sends its
                 * flips as this marker word, which never reaches
                 * gcm_flip_request -- count it here. */
                rsx_fps_stats_on_flip();
            } else {
                cellGcmSetFlipCommand(w & 0xFFu);
            }
            /* GCM_COMPOSITE_RT=<off>: a title that composites its final
             * image with the RSX 2D engine assembles it in guest memory --
             * the nv3089 CPU blit moves zeros because the source surface
             * lives on the GPU. Arm the backend's RT->backbuffer copy so
             * the flip-gated present shows the finished frame. */
            { static u32 comp = 0; static int comp_init = 0;
              if (!comp_init) { comp_init = 1;
                  const char* e = rsx_env_once("GCM_COMPOSITE_RT");
                  comp = e ? (u32)strtoul(e, NULL, 0) : 0; }
              if (comp) {
                  extern void rsx_d3d12_composite_from_offscreen(u32 raw_off);
                  rsx_d3d12_composite_from_offscreen(comp); } }
            /* PUT can be in another segment. Its numerical distance from GET
             * does not measure backlog. Present this frame before consuming
             * commands after the flip, even when later frames are submitted. */
            why = "flip";
            s_flip_hold = 1;
            break;
        }

        if (type == 1) {                       /* JUMP: 0x20000000 | offset */
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stderr, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, w & 0x1FFFFFFCu, put); }
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) {
                  gcm_fifo_bad_branch("JUMP", tgt, w);
                  gcm_fifo_dump_around(s_fifo_getoff);
                  why = "unmapped-JUMP";
                  break;
              }
              /* A self-JUMP parks the consumer until the producer patches
               * this word. PUT is an endpoint, not a replacement branch.
               * Moving PUT does not authorize skipping the intervening
               * commands or publishing their references. */
              if (tgt == s_fifo_getoff) {
                  why = "jump-park";
                  break;
              }
              s_fifo_getoff = tgt; }
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) {
                  gcm_fifo_bad_branch("CALL", tgt, w);
                  gcm_fifo_dump_around(s_fifo_getoff);
                  why = "unmapped-CALL";
                  break;
              }
              s_fifo_calloff = s_fifo_getoff + 4;
              s_fifo_getoff  = tgt; }
            continue;
        }
        if (w == 0x00020000u) {                /* RET */
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = 0;
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            /* TCONST_FIFO=1: how the title actually streams vertex constants.
             * NV4097_SET_TRANSFORM_CONSTANT is a 32-register window; if a block
             * arrives NON-INCREMENTING then every dword carries the same method
             * and any handler that derives the constant slot from the method
             * offset writes them all on top of each other. */
            { static int tf = -1;
              if (tf < 0) { const char* e = getenv("TCONST_FIFO"); tf = e ? atoi(e) : 0; }
              if (tf && subch == 0) {
                  static u32 hist[2048]; static u32 tot[2048]; static int dumped = 0;
                  u32 mi = (method >> 2) & 2047;
                  hist[mi]++; tot[mi] += count;
                  static u64 seen = 0;
                  if (++seen == 200000ull && !dumped) { dumped = 1;
                      fprintf(stderr, "[TCFIFO] method histogram (blocks, dwords):%c", 10);
                      for (u32 k = 0; k < 2048; k++) if (hist[k])
                          fprintf(stderr, "[TCFIFO]   0x%04X  %8u blocks %10u dwords%c",
                                  k << 2, hist[k], tot[k], 10);
                  }
              } }
            /* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
             *
             * These were falling through to rsx_process_method as "unknown method" --
             * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
             * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
             * RSX is supposed to write and never sees it move, so it never appends its
             * next FIFO segment and the GPU sits on a park forever.
             *
             * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
             * the method, so the next pass re-reads it -- which is what the hardware does
             * while it waits. Semaphore offsets index the same label window
             * cellGcmGetLabelAddress hands out. */
            if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) {
                int sem_blocked = 0;
                for (u32 i = 0; i < count; i++) {
                    u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea) break;
                    u32 m = (type == 0) ? method + i * 4 : method;
                    u32 v = vm_read32(dea);
                    u32 la = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
                    { static int sn = 0; if (rsx_env_once("GCM_RECDBG") && sn++ < 12)
                        fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X\n", m, v, s_sema_offset); }
                    if (m == 0x64u)      s_sema_offset = v;
                    else if (m == 0x6Cu) vm_write32(la, v);
                    else if (m == 0x68u && vm_read32(la) != v) {
            /* GCM_SEMA_ACQUIRE=1 makes ACQUIRE actually block, which is what
             * the hardware does. Off by default: a title whose label nothing
             * ever writes would wedge the drain permanently, and RELEASE on
             * its own is the half that unblocks a waiting guest. */
            static int blk = -1;
            if (blk < 0) { const char* e = getenv("GCM_SEMA_ACQUIRE"); blk = e ? atoi(e) : 0; }
            if (blk) { sem_blocked = 1; break; }
        }
                }
                if (sem_blocked) break;
                s_fifo_getoff += 4 + count * 4;
                continue;
            }
            for (u32 i = 0; i < count; i++) {
                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                if (!dea) break;
                u32 m = (type == 0) ? method + i * 4 : method;
                /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
                 * splits into subchannel 7 / method 0x0B00 -- so it was landing
                 * in the 2D engine path and being dropped as unrecognised. It
                 * is not a 2D method at all: it tells the RSX to raise a
                 * USER_CMD interrupt, which is what ps1_netemu's flip path
                 * blocks on. */
                { static int sd = -1; static unsigned long s7 = 0;
                  if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
                  if (sd && subch == 7 && (s7++ < 16))
                      fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n",
                              m, vm_read32(dea)); }
                if (subch == 7 && m == 0x0B00u) {
                    extern void rsx_raise_user_cmd(u32 arg);
                    rsx_raise_user_cmd(vm_read32(dea));
                    continue;
                }
                /* GCM_SUBCH1_3D=1: treat subchannel 1 as the 3D object too.
                 * The title issues NV4097 methods on it -- 0x1A80..0x1AC0 is
                 * SET_TEXTURE_OFFSET for units 4-6 -- and the 2D path silently
                 * discards whatever it does not recognise, so that state never
                 * reaches the renderer. SET_OBJECT binds are not tracked, which
                 * is what makes the subchannel-to-engine mapping a guess. */
                /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
                 * title binds to a subchannel is what says which engine that
                 * subchannel drives; comparing subchannel 1 against subchannel 0
                 * turns the routing below from a guess into a reading. */
                { static int od = -1;
                  if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
                  if (od && m == 0) {
                      static unsigned seen[8]; static int have[8];
                      const unsigned h = vm_read32(dea);
                      if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                          have[subch] = 1; seen[subch] = h;
                          printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                                 subch, h, 10); fflush(stdout);
                      } } }
                /* The subchannel is a binding slot, not an engine selector: a
                 * title may bind NV4097 to something other than subchannel 0.
                 * Twisted Metal uses subchannel 1, and caner (canersaka) hit
                 * the same thing in Yakuza Dead Souls, where SPU-built command
                 * lists bind NV4097 elsewhere -- his rsx_live_draw.c masks the
                 * subchannel out of the method for exactly this reason.
                 *
                 * gcm_2d_method only ever handles subchannels 2..7 (NV3062,
                 * NV308A/NV309E, NV3089), so anything arriving on 1 was being
                 * dropped on the floor -- for this title that included
                 * SET_SHADER_PROGRAM, leaving one stale fragment program bound
                 * for every draw in the game. Treat the subchannels the 2D path
                 * does not claim as 3D. GCM_SUBCH1_2D=1 restores the old split.
                 */
                static int s1_2d = -1;
                if (s1_2d < 0) { const char* e = getenv("GCM_SUBCH1_2D"); s1_2d = e ? atoi(e) : 0; }
                /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
                 * non-zero subchannel -- what each binding slot actually carries. */
                { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
                  if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
                  const u32 cm3 = m & 0x1FFCu;
                  /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
                  { static u32 vw = 1;
                    if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
                    if (vw && vm_read32(dea) == vw) {
                        static int vn; if (vn++ < 64)
                            printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, s_fifo_getoff, 10);
                    } }
                  /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
                  if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                                       cm3 == 0x1A20)) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, vm_read32(dea), 10);
                  }
                  if (sg && subch) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, vm_read32(dea), 10);
                  } }
                /* Mirror the whole method stream into the live NV4097->D3D12
                 * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
                 * It wants the raw method with its subchannel bits -- it
                 * canonicalises with & 0x1FFC itself. */
                { static int live = -1;
                  if (live < 0) live = rsx_live_draw_enabled();
                  if (live) rsx_live_draw_method((subch << 13) | m, vm_read32(dea)); }

                /* And into the platform-neutral register-file draw engine,
                 * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
                 * It wants the raw method with its subchannel bits for the
                 * same reason the live engine does, and masks them itself. */
                { static int eng = -1;
                  if (eng < 0) eng = rsx_draw_engine_enabled();
                  if (eng) rsx_draw_engine_method((subch << 13) | m, vm_read32(dea)); }

                /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
                 * for any run, live engine or not, so the method stream and the
                 * backend's draw callbacks can be compared in one measurement. */
                { static int fdbg = -1;
                  if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
                  if (fdbg) {
                      static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
                      const u32 cm = m & 0x1FFCu;
                      if      (cm == 0x1808u) n_be++;
                      else if (cm == 0x1814u) n_va++;
                      else if (cm == 0x1824u) n_ia++;
                      if (((++tick) % 200000) == 0)
                          printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                                 n_be, n_va, n_ia), fflush(stdout);
                  } }

                /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
                 * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
                 * truncates them.  Reconstruct the full address the same way
                 * the draw-engine call above does. */
                { const u32 mfull = (subch << 13) | m;
                if (mfull == 0xEB00u || mfull == 0xEB04u) {
                    cellGcmQueueUserCommand(vm_read32(dea));
                } else if (mfull == 0xE920u || mfull == 0xE924u) {
                    cellGcmSetFlipCommand(vm_read32(dea) & 7u);
                } else if (subch == 0 || (subch == 1 && !s1_2d)) {
                    /* Subchannel 1 is libgcm's NV0039 (buffer copy) slot,
                     * but some titles drive a second 3D object there instead
                     * (the branch above). NV0039 only consumes methods after
                     * a copy has fired, so 3D traffic is unaffected. */
                    if (subch == 1 && gcm_nv0039_method(m, vm_read32(dea)))
                        continue;
                    rsx_process_method(&s_state, m, vm_read32(dea));
                    /* NV406E_SET_REFERENCE: queue the fence value for PACED
                     * publication (gcm_ref_publish below) instead of letting a
                     * later fence in the same batch overwrite it. */
                    if (m == 0x50) gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff);
                    /* NV4097_GET_REPORT: (type << 24) | offset into the report
                     * area. Sample counts are not measured, so a ZPASS query
                     * reports "visible" -- the conservative answer; reading 0
                     * tells the title every queried object is occluded. */
                    if (m == 0x1800) {
                        const u32 v = vm_read32(dea), idx = (v & 0xFFFFFFu) / 16u;
                        { static int rn = 0; if (rn++ < 6) printf("[GET_REPORT] type=%u idx=%u%c", v >> 24, idx, 10); }
                        if (idx < CELL_GCM_MAX_REPORT_COUNT) {
                            s_report_data[idx].timestamp = get_timestamp_ns();
                            s_report_data[idx].value = (v >> 24) == 1u ? 0xFFFFu : 0u;
                        }
                    }
                } else
                    gcm_2d_method(subch, m, vm_read32(dea));
                }
            }
            s_fifo_getoff += 4 + count * 4;
            continue;
        }
        { static int _uw = 0; if (_uw++ < 16 && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
    }

    /* A stalled or cyclic command stream is not evidence of a new start
     * address. Keep GET at the decoded position. A later pass can observe a
     * producer patch, and an explicit driver reset can select another FIFO. */
    if (budget <= 0 && s_fifo_getoff != put)
        why = "word-budget";

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    if (rsx_env_once("GCM_DRAINDBG")) {
        static int n2 = 0;
        if (n2++ % 60 == 0)
            fprintf(stderr, "[DRAINEND] %s: %08X -> %08X (%u bytes), put=%08X\n",
                    why, start_off, s_fifo_getoff, s_fifo_getoff - start_off, put);
    }

    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    /* GCM_GET_EQ_PUT=1: publish `get` as having reached `put` rather than where
     * the walker actually is. A probe, not a fix -- it removes the back-pressure
     * a title uses to avoid overwriting commands the GPU has not read. It exists
     * to answer one question for a title that reports its ring full while the
     * walker says it is drained: is the title reading `get`, or something else?
     * If the overflow survives this, `get` is not what it consults. */
    { static int eq = -1;
      if (eq < 0) { const char* e = getenv("GCM_GET_EQ_PUT"); eq = e ? atoi(e) : 0; }
      vm_write32(GCM_CONTROL_GUEST_ADDR + 4, eq ? put : s_fifo_getoff); }

    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    u16 io_page = s_io_address_table[page];
    if (io_page == 0xFFFF) return 0xFFFFFFFFu;
    return ((u32)io_page << 20) | (ea & 0xFFFFFu);
}

void cellGcm_fifo_recycle(u32 ctx_ea)
{
    if (!ctx_ea) return;
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (current <= begin) return;                       /* nothing to recycle */

    if (rsx_env_once("CELLMARK_BLINKDBG"))
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stderr, "[REC>] tid=%lu begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* Do what the SDK's default command-buffer-full callback does: append a
     * JUMP-to-begin at the write head and move `put` to begin. The FIFO walker
     * consumes the tail, follows the jump, and idles at begin; then it's safe
     * for the guest to write from begin (that region was consumed long ago). */
    u32 io_begin = gcm_ea2io(begin);
    if (io_begin != 0xFFFFFFFFu) {
        vm_write32(current, 0x20000000u | io_begin);            /* JUMP begin  */
        /* The jump has to be in guest memory before `put` moves behind the
         * walker's `get`. Program order is not enough on arm64: the walker
         * may observe the new `put`, find `get` != `put`, and read the old
         * word at `current` before the jump lands there. */
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);        /* put = begin */
    }

    /* Wait (bounded ~2s) for the walker to consume the tail + take the jump so
     * no commands are lost; a stalled ticker degrades to dropped commands. */
    int spins = 0;
    while (g_gcm_fifo_drained_ea != begin && spins < 2000) {
#ifdef _WIN32
        gcm_sleep1_precise();   /* Sleep(1) is a 15.6 ms tick in background */
#else
        Sleep(1);
#endif
        spins++; }
    if (spins >= 2000) {
        static int warned = 0;
        if (warned++ < 4)
            printf("[cellGcmSys] fifo recycle: drain stalled (drained=0x%08X begin=0x%08X)\n",
                   g_gcm_fifo_drained_ea, begin);
    }

    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */

    if (s_recdbg)
        fprintf(stderr, "[REC<] tid=%lu cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
}

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Tell the live engine too, or it has no registered scanout to present and
     * falls back to whatever surface happens to be current. Display buffers are
     * always in RSX local memory (location 0). */
    if (rsx_live_draw_enabled())
        rsx_live_draw_set_display_buffer(bufferId, 0, offset, pitch, width, height);
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_set_display_buffer(bufferId, 0, offset, pitch, width, height);

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* NID: 0xEAA52F23 */
/* How many display buffers the title has actually registered. cellResc needs
 * it to know how far to rotate its flip target; nothing else about the set is
 * anyone else's business, so this stays a count rather than exposing the array. */
u32 cellGcm_display_buffer_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i]) n++;
    return n;
}

static s32 gcm_flip_request(u32 bufferId, int in_fifo)
{
    /* GCM_FLIPCOUNT=1: every flip, with a timestamp. FLIP_DBG caps at 20 lines,
     * which answers "did it ever flip?" and not "is it still flipping, and how
     * fast?" -- the question that matters when a title stops appearing on
     * screen. Virtua Fighter 5 does not use the in-FIFO 0xFEADxxxx flip word at
     * all (that counter stays at zero for the whole run); it flips through
     * cellGcmSetFlip, so this is where its frame rate actually is. */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      if (dbg) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long n = 0, t0 = 0;
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          ++n;
          if (n <= 400ull || (n % 10ull) == 0)
              fprintf(stderr, "[flip] %llu at %llu ms%c", n, now - t0, 10);
      } }

    /* GCM_FLIP_BT=<n>: dump the flipping thread's guest stack for the flips
     * around n. When a title renders correctly for a while and then stops
     * submitting -- no error, no unresolved import, no overflow -- the only
     * thing separating the last good frame from the first missing one is what
     * the render path was doing on each. This is the window onto that. */
    { static long long at = -2;
      if (at == -2) { const char* e = getenv("GCM_FLIP_BT"); at = e ? atoll(e) : -1; }
      if (at >= 0) {
          static unsigned long long m = 0;
          ++m;
          if ((long long)m >= at - 5 && (long long)m <= at + 5) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (g_active_ctx) {
                  char tag[48];
                  snprintf(tag, sizeof tag, "flip#%llu", m);
                  ppu_dump_guest_stack(g_active_ctx, tag);
                  { extern void ppu_dump_bctrl_ring(uint32_t, const char*);
                    ppu_dump_bctrl_ring((uint32_t)g_active_ctx->thread_id, tag); }
              }
          }
      } }

    { static int _n=0; if (rsx_env_once("FLIP_DBG") && _n++ < 20)
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* FPS_STATS=1: every accepted flip request counts once here -- import
     * cellGcmSetFlip, cellGcmSetFlipCommand, SetPrepareFlip, wait-label, and
     * non-marker FIFO flip words all funnel through this function. The FIFO
     * GCM_FLIP_MARKER word, which bypasses it, counts in the 0xFEAD handler. */
    rsx_fps_stats_on_flip();

    /* in_fifo: the flip sits in the command stream as a GCM_FLIP_MARKER, and
     * the drain does this half when it reaches it (see the 0xFEAD case). */
    /* Flip requested but not yet shown: a subsequent cellGcmSetWaitFlip blocks
     * until the present thread's cellGcmTickFlip marks it done (vsync).
     * This request opens its own wait generation; a direct flip is "drained"
     * at once, a FIFO one when the drain reaches its marker. */
    AcquireSRWLockExclusive(&s_flip_lock);
    gcm_flip_wait_opened();
    if (!in_fifo) {
        s_current_display_buffer_id = bufferId;
        s_flip_drained_gen = s_flip_wait_gen;
        s_flip_pending = 1;   /* ticker: present BEFORE the next drain */
        s_flip_request_count++;
    }
    ReleaseSRWLockExclusive(&s_flip_lock);
    s_last_flip_time = get_timestamp_ns();

    /* Invoke via OPD resolution, not a raw call into guest code.
     *
     * On hardware this callback fires when the flip COMPLETES, at vsync --
     * which is what cellGcmTickFlip does. Calling it again here, at request
     * time, is a compatibility wart: some titles need the early edge, but a
     * title that issues its first flip while still initialising then runs its
     * handler against half-built state. Tokyo Jungle faults that way, in
     * func_001F2D3C, dereferencing a singleton it has not constructed yet.
     * GCM_FLIPCB_ONTICK=1 leaves the callback to the tick alone. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);  /* head 0 = primary display */

    return CELL_OK;
}

s32 cellGcmSetFlipCommand(u32 bufferId) { return gcm_flip_request(bufferId, 0); }

/* The real _cellGcmSetFlipCommand appends the flip to the command buffer, so
 * the RSX flips once it has executed everything queued before it. Firing it
 * at call time instead presented whatever the drain had reached by then --
 * and the title has not even flushed its frame's tail yet: The Simpsons Arcade
 * Game calls it with ctx->current ~2.4 KB past put, so frames showed without
 * their last draws (a dialog, a dimming overlay) for one flip. Write a marker
 * at ctx->current instead; the drain stops at it and fires the flip there,
 * exactly as it does for the 0xFEAD words a statically-linked libgcm emits.
 * Returns 0 when there is no room or ctx is not a mapped command buffer. */
static int gcm_flip_into_fifo(u32 ctx, u32 bufferId)
{
    if (!ctx || bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId]) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Keep the title's reserve space before its ring wraps. */
    if (cur + 8 + GCM_RECYCLE_SLACK > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, GCM_FLIP_MARKER | bufferId);
    /* Append the wrap on the producer thread. The consumer follows the
     * emitted JUMP; it never rewinds the title's context or selects an entry
     * point from numerical GET/PUT distance. */
    const u32 begin = vm_read32(ctx), io_begin = gcm_ea2io(begin);
    if (end - (cur + 4) < GCM_FLIP_WRAP_BYTES && io_begin != 0xFFFFFFFFu &&
        s_fifo_getoff >= io_begin + GCM_RECYCLE_MARGIN) {
        vm_write32(cur + 4, 0x20000000u | io_begin);   /* JUMP -> begin */
        vm_write32(ctx + 8, begin);
        return 1;
    }
    vm_write32(ctx + 8, cur + 4);
    return 1;
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    vm_write32(GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE, labelValue);

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    s_current_display_buffer_id = bufferId;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_request_count++;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. See cellGcmSetFlipCommand for why
     * GCM_FLIPCB_ONTICK exists. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    return s_current_display_buffer_id;
}

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_flip_handler_opd = (u32)(size_t)handler;
    s_flip_handler = handler;
    { u32 c = s_flip_handler_opd ? vm_read32(s_flip_handler_opd) : 0; if (c) s_flip_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_vblank_handler_opd = (u32)(size_t)handler;
    s_vblank_handler = handler;
    { u32 c = s_vblank_handler_opd ? vm_read32(s_vblank_handler_opd) : 0; if (c) s_vblank_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
}

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    gcm_update_handler_mask();
}

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* NID: 0x0E6B0DFF */
/* Mirror the host tables into the guest window the title will index. */
static void gcm_publish_offset_tables(void)
{
    for (u32 i = 0; i < GCM_OFFSET_TABLE_PAGES; i++) {
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + i * 2, s_io_address_table[i]);
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + i * 2, s_ea_address_table[i]);
    }
}

s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    u32 ea = (u32)(uintptr_t)table;
    if (!ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_publish_offset_tables();

    /* The guest struct is two 4-byte EAs, not two host pointers. */
    vm_write32(ea + 0, GCM_OFFSET_TABLE_IO_ADDR);
    vm_write32(ea + 4, GCM_OFFSET_TABLE_EA_ADDR);
    return CELL_OK;
}

/* NID: 0xDB769B32 */
static u32 gcm_io_alloc(u32 size);   /* defined below */

s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    if (!off_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* A2O_DBG=<N>: the EA->offset conversions a title asks for. A texture bound
     * at an offset nothing ever writes usually means the offset came from an EA
     * in a space we classified wrongly, and that is only visible here. */
    { static int cap = -1, k = 0;
      if (cap < 0) { const char* e = getenv("A2O_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && k < cap) { k++;
        u32 nz = 0;
        for (u32 i = 0; i < 0x400u; i += 13) if (vm_base && vm_base[address + i]) nz++;
        printf("[A2O] ea=0x%08X  (first 1KB nonzero=%u/79)\n", address, nz); } }

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        if ((loff >> 20) < sizeof(s_local_offset_page)) s_local_offset_page[loff >> 20] = 1;
        vm_write32(off_ea, loff);
        return CELL_OK;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        vm_write32(off_ea, (io_page << 20) | (address & 0xFFFFF));
        return CELL_OK;
    }

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        vm_write32(off_ea, address - s_config.ioAddress);
        return CELL_OK;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use.
     *
     * The bound used to be a flat `< 0x40000000`, which is not a description of
     * anything -- RSX local memory is already handled at the top of this
     * function against its real base, and the raw-SPU window at 0x30000000 sits
     * *below* the old bound, so it was not keeping that out either. What it did
     * do was exclude titles whose RSX-visible heap lives high: Virtua Fighter 5
     * maps 0x4A900000..0x4B400000 and then hands us 0x4B400040, which fell past
     * 0x40000000, skipped the auto-map, and failed -- 976 times a boot, leaving
     * the title with no offsets to draw with (20 draws in five minutes).
     *
     * So the test is now what it always meant: anything below RSX local memory
     * is main memory. Local is checked first and returns above, so reaching
     * here with an address under localAddress means an unmapped main-memory
     * page, which is exactly the case this fallback exists for. */
    /* NULL is not a buffer. Auto-mapping it costs an IO page and, worse, hands
     * the title a real-looking offset for guest address 0 -- so an EA the title
     * computed as NULL (a failed allocation, an out-param it never filled)
     * silently becomes a valid RSX offset instead of an error it could notice.
     * The Orange Box's chooser asks for exactly this, and the page it burned was
     * the one the following cellGcmMapMainMemory would otherwise have taken. */
    if (address < 0x100000u) {
        printf("[cellGcmSys] AddressToOffset REFUSED: ea 0x%08X is not a mappable "
               "address (page 0)\n", address);
        vm_write32(off_ea, 0);
        return CELL_GCM_ERROR_INVALID_VALUE;
    }

    u32 auto_map_limit = s_config.localAddress ? s_config.localAddress : 0x40000000u;
    if (address < auto_map_limit) {
        u32 ea_page = address & ~0xFFFFFu;
        u32 io      = gcm_io_alloc(0x100000u);
        populate_offset_table(ea_page, io, 0x100000u);
        /* Record it in the mapping list too, not just the offset table.
         *
         * These are two different structures answering two different questions,
         * and only the offset table was being filled in. The FIFO walker
         * resolves io -> ea through the MAPPING LIST (find_io_mapping_by_io),
         * so an auto-mapped range was invisible to it: a JUMP into one reported
         * "no IO mapping", the walker resynced straight to put, and every fence
         * in the skipped range was lost -- which strands a title spinning on
         * ctrl->ref with no way to see why. Both paths have to agree about what
         * is mapped. */
        IoMapping* m = find_free_mapping();
        if (m) {
            m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
            s_io_mapping_count++;
        } else {
            printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                   "the FIFO will not be able to walk io 0x%08X\n", io);
        }
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        vm_write32(off_ea, io | (address & 0xFFFFFu));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    vm_write32(off_ea, 0);
    { static int _d = -1; if (_d < 0) _d = getenv("A2O_FAILDBG") ? 1 : 0;
      if (_d) { static int _n = 0; if (_n++ < 12)
          fprintf(stderr, "[A2O] FAILED ea=0x%08X (not in local or IO space)%c",
                  address, 10); } }
    return CELL_GCM_ERROR_FAILURE;
}

/* IO-offset space bump allocator for main-memory mappings. Starts PAST the
 * Init command-buffer region: handing out io 0 (the old s_local_mem_allocated
 * bump, which begins at 0) remapped the FIFO's own io page -- demosaic's
 * cellGcmMapMainMemory(sys heap) got offset 0, io2ea(0) then resolved into its
 * heap (all zeros) and the RSX walker consumed every frame's commands as
 * no-ops (no draws, label fence never written, guest waited forever). */
static u32 s_io_alloc_next = 0;
static u32 gcm_io_alloc(u32 size)
{
    if (s_io_alloc_next == 0)
        s_io_alloc_next = (s_config.ioSize + 0xFFFFFu) & ~0xFFFFFu;
    if (s_io_alloc_next < 0x100000u) s_io_alloc_next = 0x100000u;
    u32 io = s_io_alloc_next;
    s_io_alloc_next += (size + 0xFFFFFu) & ~0xFFFFFu;
    return io;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    /* Log the REQUEST before validating it. Every rejection below used to
     * happen before the only printf in this function, so a title whose
     * mapping we refused looked -- in the log -- like a title that never asked.
     * Virtua Fighter 5 then fails cellGcmAddressToOffset ~976 times a boot for
     * addresses immediately past the region it did get, draws 20 times instead
     * of thousands, and never flips; the refusal that would explain it was
     * invisible. Same lesson as the savedata early returns. */
    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    if (!offset) {
        printf("[cellGcmSys] MapMainMemory REFUSED: offset out-param is NULL\n");
        return CELL_GCM_ERROR_INVALID_VALUE;
    }
    /* Size and EA must both be 1MB aligned. */
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if ((ea & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: ea 0x%08X is not 1MB aligned\n", ea);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }

    /* Allocate a fresh IO region past the Init command-buffer window. */
    u32 io_offset = gcm_io_alloc(size);

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] WARNING: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io_offset, size);

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Logged before validation, and every failure path named -- same reason as
     * cellGcmMapMainMemory above. Two of these returned silently AFTER the log
     * line as well, so even a call that got that far could vanish. */
    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X / io 0x%08X not 1MB aligned\n", ea, io);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (find_mapping_by_ea(ea) != NULL) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X is already mapped\n", ea);
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io, size);

    return CELL_OK;
}

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    IoMapping* mapping = find_mapping_by_ea(ea);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        return s_config.localAddress + offset;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return s_config.localAddress + offset;
}

/* IO-table-first resolve: skip the local-page shortcut and ask the IO offset
 * table, falling back to VRAM only if the page is unmapped. cellGcmResolveOffset
 * prefers local for any page the guest ever derived from a local EA, which is
 * right for a title that really does keep the data in VRAM but wrong for one
 * that builds its textures in main memory on pages whose numbers happen to
 * collide. Returns 0 when the IO table has no mapping, so a caller can tell
 * "not IO-mapped" from "IO-mapped at 0". */
u32 cellGcmResolveIO(u32 offset)
{
    u32 page = offset >> 20;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return 0;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local)
        return s_config.localAddress + offset;
    return cellGcmResolveOffset(offset);
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (index >= CELL_GCM_MAX_LABEL_COUNT) {
        printf("[cellGcmSys] WARNING: GetLabelAddress index %u out of range\n", index);
        return NULL;
    }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* NID: 0x97FC4B73 */
/* NID: 0x99D397AC. The value NV4097_GET_REPORT stored at this index (the
 * walker fills it). Unregistered, every call returned a faked 0. */
u32 cellGcmGetReport(u32 type, u32 index)
{
    { static int n = 0;
      if (n++ < 8 || (n % 2000) == 0) printf("[cellGcmSys] GetReport(type=%u, index=%u) -> %u%c", type, index,
                          index < CELL_GCM_MAX_REPORT_COUNT ? s_report_data[index].value : 0u, 10); }
    if (index >= CELL_GCM_MAX_REPORT_COUNT) return 0;
    return s_report_data[index].value;
}

u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u)\n",
           index, location, offset, size, pitch);

    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;

    return CELL_OK;
}

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;

    return CELL_OK;
}

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    s_tiles[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u)\n", index);
    s_zcull[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    s_tiles[index].bound = 0;
    return CELL_OK;
}

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x107BF789
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (_n++<4) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data area — similar to report data, 16 bytes per slot */
static u8 s_notify_data[256 * 16];

void* cellGcmGetNotifyDataAddress(u32 index)
{
    if (index >= 256) return NULL;
    return &s_notify_data[index * 16];
}

/* Timestamp location — returns CELL_GCM_LOCATION_LOCAL or MAIN */
/* The Location suffix names an ARGUMENT, not an out-parameter: it is the
 * caller saying which memory the report lives in, CELL_GCM_LOCATION_LOCAL or
 * _MAIN, exactly as cellGcmGetReportDataLocation and
 * cellGcmGetReportDataAddressLocation take it three hundred lines below. This
 * one alone was declared u32* and treated the value as a guest address to
 * write the location INTO, which inverts the parameter and loses the return.
 *
 * A title passing CELL_GCM_LOCATION_MAIN therefore had a 1 taken for an
 * effective address, and vm_write32 stored four bytes at guest address 1.
 * Yakuza: Dead Souls does that during display setup and dies there.
 *
 * It returns the timestamp, like cellGcmGetTimeStamp, whose body this is with
 * a location the report table does not distinguish. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID 0x21397818 is gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
/* NID: 0x21397818 */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    if (gcm_flip_into_fifo((u32)(uintptr_t)ctx, bufferId))
        return gcm_flip_request(bufferId, 1);
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD8F88E1A */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    (void)ctx;
    return cellGcmSetFlipCommandWithWaitLabel(bufferId, labelIndex, labelValue);
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Guest out-params, like every other pointer parameter in this file. */
    vm_write32((uint32_t)(uintptr_t)address, s_config.localAddress);
    vm_write32((uint32_t)(uintptr_t)size,    s_config.localSize);

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           s_config.localAddress, s_config.localSize);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    s_gcm_initialized = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_user_handler     = NULL;
    s_second_v_handler = NULL;
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_last_flip_time = 0;
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* Return tile info for a given index */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

/* Return zcull info for a given index */
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;

    /* On hardware this re-points gCellGcmCurrentContext at the default context
     * cellGcmInit built. Zeroing a host-side struct is not that: a title that
     * calls it to go back to the big default buffer stays on whatever smaller
     * segment it had switched to.
     *
     * GCM_DEFAULT_CTX_REPOINT=1 does the real thing -- write our context's EA
     * into the guest variable cellGcmInit's ctx_out pointed at, and rewind that
     * context. Off by default because a title managing its own segment chain
     * may hold pointers into the old one; this exists to be measured, not
     * assumed.
     *
     * Measured once now, on Virtua Fighter 5, which is the case this was
     * written for. VF5's AMGL keeps its own command buffers (it repoints
     * gCellGcmCurrentContext at a 512 KB segment of its own inside the 4 MB it
     * maps) and calls this entry to recover when one fills. Getting a host-side
     * struct zeroed instead leaves it on the exhausted segment, so it allocates
     * another, fills that, and asks again:
     *
     *     [AMGL]:[ERROR] Command Buffer Overflow!   x91, 636 calls to here
     *
     * With the repoint: **0 overflows**, and the command packets reaching the
     * draw engine go from 12,756 to 15,097 over the same five minutes. Still
     * one title's evidence, so still opt-in -- but it is no longer unmeasured,
     * and a port whose title manages its own segments should try it. */
    if (getenv("GCM_DEFAULT_CTX_REPOINT") && s_gcm_ctx_out_ea && s_gcm_context_ea) {
        vm_write32(s_gcm_context_ea + 0x8, vm_read32(s_gcm_context_ea + 0x0));
        vm_write32(s_gcm_ctx_out_ea, s_gcm_context_ea);
        printf("[cellGcmSys] SetDefaultCommandBuffer: repointed "
               "gCellGcmCurrentContext@0x%08X -> 0x%08X\n",
               s_gcm_ctx_out_ea, s_gcm_context_ea);
    }
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* Default command word size: 0x400 (1024) words */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return 0x400;
}

/* Default segment word size: 0x100 (256) words */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return 0x100;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    AcquireSRWLockExclusive(&s_flip_lock);
    if (status == CELL_GCM_FLIP_STATUS_WAITING) gcm_flip_wait_opened();
    else s_flip_status = status;
    ReleaseSRWLockExclusive(&s_flip_lock);
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
}

/* Called when a FIFO consumer retires a user-interrupt method. Publishing
 * the cause and pending marker is atomic; guest code runs later in the pump.
 * Like the driver cause register, multiple pending commands coalesce. */
void cellGcmQueueUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
    GCM_USER_PENDING_STORE((1ULL << 32) | cmd);
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    (void)location;  /* We only have one report area */

    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    return s_report_data[index].value;
}

/* ---------------------------------------------------------------------------
 * VRAM upload registry
 *
 * A title whose texture uploads land at an address that does not match the
 * offset it later programs into SET_TEXTURE_OFFSET leaves the sampler reading
 * untouched memory. Recording what was uploaded lets the texture path find the
 * real bytes by size instead of trusting the offset. Diagnostic bridge: it is
 * a lookup, not a fix for the offset bookkeeping itself.
 * -----------------------------------------------------------------------*/
#define RSX_UPLOAD_LOG 256
static struct { u32 dst, size; int claimed; } s_uploads[RSX_UPLOAD_LOG];
static u32 s_upload_n;

void rsx_note_vram_upload(u32 dst_ea, u32 size)
{
    if (!size) return;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].dst     = dst_ea;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].size    = size;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].claimed = 0;
    s_upload_n++;
}

/* OLDEST unclaimed upload of exactly `want` bytes, claimed on return. Matching
 * by size alone hands every same-sized surface the same image; consuming them in
 * order pairs the Nth bind of a size with the Nth upload of that size, which is
 * the order a title loads and then draws its materials in. Returns 0 if none. */
u32 rsx_find_vram_upload(u32 want)
{
    u32 n = s_upload_n < RSX_UPLOAD_LOG ? s_upload_n : RSX_UPLOAD_LOG;
    u32 first = s_upload_n > RSX_UPLOAD_LOG ? s_upload_n - RSX_UPLOAD_LOG : 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (first + i) % RSX_UPLOAD_LOG;
        if (s_uploads[idx].size == want && !s_uploads[idx].claimed) {
            s_uploads[idx].claimed = 1;
            return s_uploads[idx].dst;
        }
    }
    return 0;
}

/* Let the same pairing repeat next frame. */
void rsx_reset_upload_claims(void)
{
    for (u32 i = 0; i < RSX_UPLOAD_LOG; i++) s_uploads[i].claimed = 0;
}

/* ---------------------------------------------------------------------------
 * sys_rsx_* bridge (libs/video/sys_rsx.c)
 *
 * A PS3 firmware module links libgcm statically and drives RSX through the lv2
 * syscalls instead of importing cellGcmSys, so none of the HLE entry points above
 * ever run. Everything the FIFO walker needs is still here -- it just has to be
 * reachable from the syscall layer. These four accessors are that surface; the
 * walker, the method decoder and rsx_live_draw are untouched.
 * -----------------------------------------------------------------------*/

/* Bring the GCM state up the way cellGcmInit would, minus the IO window (the
 * driver maps that itself with sys_rsx_context_iomap). Returns the guest base of
 * RSX local memory, which is what sys_rsx_memory_allocate hands back. */
u32 cellGcm_syscall_bringup(u32 local_size)
{
    if (!s_gcm_initialized)
        cellGcmInit(0x10000, 0, 0);
    if (local_size)
        s_config.localSize = local_size;
    s_gcm_syscall_mode = 1;
    return s_config.localAddress;
}

/* Guest EA of the put/get/ref triple. The driver gets this as
 * sys_rsx_context_allocate's lpar_dma_control + 0x40 (verified in the image:
 * `lwz r3,0x18(r9)` / `addi r3,r3,64`), so the syscall returns this minus 0x40
 * and the driver's own flushes land exactly where the walker reads. */
u32 cellGcm_control_guest_addr(void) { return GCM_CONTROL_GUEST_ADDR; }

/* sys_rsx_context_iomap / iounmap: the same 1MB-page offset table
 * cellGcmMapEaIoAddress fills, which is what gcm_io2ea() and every offset
 * resolution read. */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
{
    populate_offset_table(ea, io, size);
    if (s_io_mapping_count < CELL_GCM_MAX_IO_MAPPINGS) {
        s_io_mappings[s_io_mapping_count].ea     = ea;
        s_io_mappings[s_io_mapping_count].io     = io;
        s_io_mappings[s_io_mapping_count].size   = size;
        s_io_mappings[s_io_mapping_count].active = 1;
        s_io_mapping_count++;
    }
    if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
}

void cellGcm_syscall_iounmap(u32 io, u32 size)
{
    u32 page = io >> 20;
    u32 ea = (page < 65536 && s_ea_address_table[page] != 0xFFFF)
             ? ((u32)s_ea_address_table[page] << 20) : 0;
    if (ea) clear_offset_table(ea, io, size);
}

/* sys_rsx_context_attribute(0x001): the driver's explicit FIFO reset. Move the
 * walker with it -- leaving s_fifo_getoff where it was makes the next drain walk
 * from a stale offset through whatever the driver has since overwritten. */
void cellGcm_syscall_set_fifo(u32 put, u32 get)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get);
    s_fifo_getoff  = get;
    s_fifo_calloff = 0;
}

/* _cellGcmFunc15 (NID 0x3A33C1FD): SDK-internal context hook the library
 * import table references. Firmware performs no visible work for it, so it
 * is a logged no-op -- but it must exist or the import lands on the
 * unresolved-NID default path. */
void _cellGcmFunc15(void* context)
{
    (void)context;
}
