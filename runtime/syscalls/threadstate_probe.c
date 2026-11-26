/*
 * ps3recomp - RD_PROBE_THREADSTATES=1 thread-state sampler.
 *
 * A title that boots and then goes quiet is usually not hung: one guest
 * thread is blocked on a primitive nothing ever signals, and every other
 * thread is parked behind it. This probe answers "which thread, where,
 * waiting on what" from evidence instead of guesswork: every 5 s it prints
 * one line per live PPU thread --
 *
 *   tid / name / cia (guest VA of the last syscall or HLE callsite, the
 *   dispatcher's prof stamp) / wait tag (the primitive the thread is inside
 *   right now, stamped by ppu_thread_wait_begin) / host CPU delta since the
 *   previous sample (a climbing delta with a frozen cia means the thread is
 *   spinning in recompiled code, not blocked in HLE).
 *
 * It is a plain host thread that READS the scheduler's bookkeeping
 * (g_ppu_threads via the ppu_prof_snapshot/ppu_thread_wait_tag accessors):
 * no locks are taken on the sampled path, no guest thread is suspended, so
 * it cannot perturb the timing of the run it observes.
 */

#include "sys_ppu_thread.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* sys_ppu_thread.c */
extern int ppu_prof_snapshot(int idx, unsigned* tid, unsigned* cia, const char** name);
extern const char* ppu_thread_wait_tag(unsigned tid);
extern unsigned long long ppu_thread_cpu_us(unsigned tid);
extern void* ppu_thread_host_handle(unsigned tid);
extern unsigned ppu_prof_resolve_host(void* ra);   /* host -> guest fn VA */
static void probe_backtrace(unsigned tid, char* out, size_t outsz);

/* ---------------------------------------------------------------------------
 * RD_PROBE_MAINSPIN: one-shot dead-boot discriminator (MotorStorm 1,
 * BCES00006). Signature from the 75 s dead boot: main pinned at
 * cia=0x00D31498 (the per-frame flip/submit step: cellGcmSetFlip +
 * cellGcmSetWaitFlip + FEED-ring writes), no wait tag, >=1 s host CPU per
 * 5 s round, two rounds running. On fire, freeze the main thread and dump
 * the guest state that spin watches:
 *   [r2-0x274] flip-state object  (+0x1C == 2 takes the early path)
 *   [r2-0x270][0]<<4 + [r2-0x26C] -> command ring (+4 end, +8 put, +0xC
 *     flush-callback OPD {fn,toc}; D28B48 ps3_indirect_call's it on wrap)
 *   [r2-0x268] wrap watermark (+0 = last published put)
 *   r18 feed ctx: +0x13 busy flag, +0x1F60/+0x1F64 pending counts
 *   r14 owner: +0x5A88 vs [+0x5A04]+0xF0 -> +0x1C0 progress counter pair
 *   GCM control put/get/ref
 * A dead-boot dump where ring.put==ring.end and cbfn lands inside
 * 0xDF150..0xEFC0C pins the ring-wrap callback as the spin site and shows
 * which counter stopped moving. Off unless RD_PROBE_MAINSPIN is set
 * (requires the RD_PROBE_THREADSTATES probe thread).
 * -----------------------------------------------------------------------*/
extern void* cellGcmGetControlRegister(void);    /* libs/video/cellGcmSys.c */
extern unsigned int g_sc_inflight[];             /* ppu_loader.cpp syscall in-flight */

#ifdef _WIN32
/* host = vm_base + guest; vm_base==0 under native-VA mapping (guest EA is
 * the host pointer). Fault-guarded on Win32 so a not-yet-mapped EA yields 0
 * instead of killing the probe. */
static unsigned int ms_rd32(unsigned int ea)
{
    if (!vm_base && !ppu_vm_size) return 0;
    const unsigned char* p = vm_base ? vm_base + ea : (const unsigned char*)(size_t)ea;
    __try { return *(const unsigned int*)p; }
    __except (1) { return 0; }
}

static unsigned char ms_rd8(unsigned int ea)
{
    if (!vm_base && !ppu_vm_size) return 0;
    const unsigned char* p = vm_base ? vm_base + ea : (const unsigned char*)(size_t)ea;
    __try { return *p; }
    __except (1) { return 0; }
}
#endif /* ms_rd helpers: SEH fault guard, Win32-only */

/* MAINSPIN_WATCH=<ea1,ea2,...> shared table (hex, comma list, max 8).
 * Parsed once on FIRST USE -- not at boot -- so the DMA-side OKPAY stamp
 * (spu_dma.h) sees the same set even before the dump ever fires. Read by
 * mainspin_dump (4 words per EA) and matched against successful PUT EAs. */
static unsigned int s_watch[8];
static int s_nwatch = -1;
static void mainspin_watch_init(void)
{
    if (s_nwatch >= 0) return;
    s_nwatch = 0;
    const char* e = getenv("MAINSPIN_WATCH");
    if (!e) return;
    const char* p = e;
    while (s_nwatch < 8) {
        char* d; unsigned long v = strtoul(p, &d, 16);
        if (d == p) break;
        s_watch[s_nwatch++] = (unsigned int)v;
        if (*d != ',') break;
        p = d + 1;
    }
}
unsigned int mainspin_watch_n(void)  { mainspin_watch_init(); return (s_nwatch > 0) ? (unsigned int)s_nwatch : 0u; }
unsigned int mainspin_watch_at(int i) { mainspin_watch_init(); return (i >= 0 && i < s_nwatch) ? s_watch[i] : 0u; }

#ifdef _WIN32
static void mainspin_dump(unsigned round, long long dcpu, const char* bt)
{
    ppu_context* c = (ppu_context*)ppu_prof_main_ctx();
    if (!c) { fprintf(stderr, "[MAINSPIN] r=%u no main ctx yet\n", round); fflush(stderr); return; }

    /* freeze main: ctx->gpr is only coherent between lifted chunks while the
     * thread is suspended; the suspend is sub-ms, same cadence as backtrace */
    HANDLE h = (HANDLE)ppu_thread_host_handle(1);
    if (h) SuspendThread(h);

    unsigned int r2  = (unsigned int)c->gpr[2];
    unsigned int r3  = (unsigned int)c->gpr[3];
    unsigned int r14 = (unsigned int)c->gpr[14];
    unsigned int r18 = (unsigned int)c->gpr[18];
    unsigned int r31 = (unsigned int)c->gpr[31];

    unsigned int s274  = ms_rd32(r2 - 0x274);
    unsigned int flip  = ms_rd32(s274 + 0x1C);
    unsigned int wm    = ms_rd32(r2 - 0x268);
    unsigned int wmput = ms_rd32(wm + 0);
    unsigned int idx0  = ms_rd32(ms_rd32(r2 - 0x270) + 0);
    unsigned int ring  = (idx0 << 4) + ms_rd32(r2 - 0x26C);
    unsigned int rend  = ms_rd32(ring + 0x4);
    unsigned int rput  = ms_rd32(ring + 0x8);
    unsigned int rcb   = ms_rd32(ring + 0xC);
    unsigned int cbfn  = ms_rd32(rcb + 0);
    unsigned int cbtoc = ms_rd32(rcb + 4);
    unsigned int feed_busy = ms_rd8(r18 + 0x13);
    unsigned int feed_1f60 = ms_rd32(r18 + 0x1F60);
    unsigned int feed_1f64 = ms_rd32(r18 + 0x1F64);
    /* V2: try every plausible mgr base. The poll loop (0xDF858) keeps its own
     * r14 in its own frame, 4 trampoline hops below the captured one, so a
     * single-base read came back zeros. The giant feed processor's mgr is one
     * of the live registers; the offsets (+0x5A04 chain, +0x5A88/+0x5A8C done,
     * queue+0x1C0/+0x1C4 produced) are from the lifted 0xDF150 head/tail. */
    unsigned int bases[5] = { r2, r3, r14, r18, r31 };
    unsigned int r7v[5], qv[5], cv[5], c4v[5], dAv[5], dBv[5];
    for (int i = 0; i < 5; i++) {
        r7v[i] = ms_rd32(bases[i] + 0x5A04);
        qv[i]  = ms_rd32(r7v[i] + 0xF0);
        cv[i]  = ms_rd32(qv[i] + 0x1C0);
        c4v[i] = ms_rd32(qv[i] + 0x1C4);
        dAv[i] = ms_rd32(bases[i] + 0x5A88);
        dBv[i] = ms_rd32(bases[i] + 0x5A8C);
    }
    /* Fixed guest EAs to dump: MAINSPIN_WATCH via the shared accessors (the
     * same table spu_dma.h OKPAY matches PUT EAs against). 4 words each. */
    unsigned int wn = mainspin_watch_n();
    unsigned int wt[8];
    for (int i = 0; i < (int)wn; i++) wt[i] = mainspin_watch_at(i);
    unsigned int wv[8][4];
    for (int i = 0; i < (int)wn; i++)
        for (int k = 0; k < 4; k++) wv[i][k] = ms_rd32(wt[i] + 4 * k);
    /* V3 stack scan: walk the guest stack from r1 (1024 slots) while still
     * suspended; record slots holding a work-area (0x4024xxxx), queue-B
     * (0x10273xxx) or watched pointer -- finds the frames that CARRIED the
     * queue/ticket pointers even when the owning register is gone. Capped 32. */
    unsigned int r1 = (unsigned int)c->gpr[1];
    unsigned int so[32], sv[32]; int sN = 0;
    for (int i = 0; i < 1024 && sN < 32; i++) {
        unsigned int v = ms_rd32(r1 + 4u * i);
        int hit = ((v >= 0x40240000u && v < 0x40250000u) ||
                   (v >= 0x10273000u && v < 0x10274000u));
        if (!hit)
            for (int w = 0; w < (int)wn; w++) if (v == wt[w]) { hit = 1; break; }
        if (hit) { so[sN] = 4u * i; sv[sN] = v; sN++; }
    }
    unsigned int gcm_put = 0, gcm_get = 0, gcm_ref = 0;
    void* ctrl = cellGcmGetControlRegister();
    if (ctrl) { gcm_put = ms_rd32((unsigned int)(size_t)ctrl);
                gcm_get = ms_rd32((unsigned int)(size_t)ctrl + 4);
                gcm_ref = ms_rd32((unsigned int)(size_t)ctrl + 8); }

    if (h) ResumeThread(h);

    fprintf(stderr, "[MAINSPIN] r=%u fired dcpu=%+lldus sc_inflight=%u\n",
            round, dcpu, g_sc_inflight[1]);
    fprintf(stderr, "[MAINSPIN]    bt %s\n", bt);
    fprintf(stderr, "[MAINSPIN] regs r2=%08X r3=%08X r14=%08X r18=%08X r31=%08X lr=%08X ctr=%08X cia=%08X\n",
            r2, r3, r14, r18, r31, (unsigned)c->lr, (unsigned)c->ctr, (unsigned)c->cia);
    fprintf(stderr, "[MAINSPIN] flip s274=%08X [s274+1C]=%08X wm=%08X [wm]=%08X\n",
            s274, flip, wm, wmput);
    fprintf(stderr, "[MAINSPIN] ring=%08X end=%08X put=%08X cb=%08X cbfn=%08X cbtoc=%08X%s\n",
            ring, rend, rput, rcb, cbfn, cbtoc,
            (rend && rput == rend) ? " FULL" : "");
    fprintf(stderr, "[MAINSPIN] feed r18=%08X busy13=%02X 1f60=%08X 1f64=%08X\n",
            r18, feed_busy, feed_1f60, feed_1f64);
    { const char* bn[5] = { "r2", "r3", "r14", "r18", "r31" };
      for (int i = 0; i < 5; i++)
          if (r7v[i])
              fprintf(stderr, "[MAINSPIN] own %s=%08X r7=[b+5A04]=%08X q=[r7+F0]=%08X"
                              " cEA=q+1C0=%08X [cEA]=%08X [cEA+4]=%08X [b+5A88]=%08X [b+5A8C]=%08X\n",
                      bn[i], bases[i], r7v[i], qv[i], qv[i] + 0x1C0, cv[i], c4v[i],
                      dAv[i], dBv[i]);
      fprintf(stderr, "[MAINSPIN] own direct [r14+5A88]=%08X [r18+5A88]=%08X\n",
              ms_rd32(r14 + 0x5A88), ms_rd32(r18 + 0x5A88)); }
    for (int i = 0; i < (int)wn; i++)
        fprintf(stderr, "[MAINSPIN] watch ea=%08X w=%08X %08X %08X %08X\n",
                wt[i], wv[i][0], wv[i][1], wv[i][2], wv[i][3]);
    for (int i = 0; i < sN; i++)
        fprintf(stderr, "[MAINSPIN] stackscan off=%X val=%08X\n", so[i], sv[i]);
    fprintf(stderr, "[MAINSPIN] gcm put=%08X get=%08X ref=%08X\n", gcm_put, gcm_get, gcm_ref);
    fflush(stderr);
}
#else
static void mainspin_dump(unsigned round, long long dcpu, const char* bt)
{
    fprintf(stderr, "[MAINSPIN] r=%u fired dcpu=%+lldus (win32-only dump) bt %s\n",
            round, dcpu, bt);
    fflush(stderr);
}
#endif

static void mainspin_consider(unsigned tid, unsigned cia, const char* wait,
                              long long dcpu, unsigned round, const char* bt)
{
    static int armed = -1, hits = 0, fired = 0;
    if (fired || tid != 1) return;
    if (armed < 0) armed = getenv("RD_PROBE_MAINSPIN") != NULL;
    if (!armed) return;
    int spinning = cia == 0x00D31498        /* MotorStorm flip/submit step */
                && (!wait || !*wait)
                && dcpu >= 1000000;         /* >=1 s CPU per 5 s round */
    hits = spinning ? hits + 1 : 0;
    if (hits >= 2) { fired = 1; mainspin_dump(round, dcpu, bt); }
}

#define PROBE_PERIOD_MS 5000

static unsigned long long s_prev_cpu[PPU_THREAD_MAX + 1];

static void probe_dump(unsigned round, unsigned long long now_ms)
{
    unsigned live = 0;
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        unsigned tid = 0, cia = 0;
        const char* name = NULL;
        if (!ppu_prof_snapshot(i, &tid, &cia, &name)) continue;
        if (tid > PPU_THREAD_MAX) continue;

        const char* wait = ppu_thread_wait_tag(tid);
        unsigned long long cpu = ppu_thread_cpu_us(tid);
        long long dcpu = (long long)(cpu - s_prev_cpu[tid]);
        s_prev_cpu[tid] = cpu;

        char bt[512];
        probe_backtrace(tid, bt, sizeof(bt));
        fprintf(stderr,
                "[TSTATE] r=%u t=%llums tid=%-2u name=%-12s cia=0x%08X wait=%-32s dcpu=%+lldus\n",
                round, now_ms, tid, (name && *name) ? name : "-",
                cia, wait ? wait : "-", dcpu);
        fprintf(stderr, "[TSTATE]    bt %s\n", bt);
        mainspin_consider(tid, cia, wait, dcpu, round, bt);
        live++;
    }
    fprintf(stderr, "[TSTATE] r=%u t=%llums live=%u\n", round, now_ms, live);
    fflush(stderr);
}

#ifdef _WIN32
static unsigned __stdcall probe_thread(void* unused)
#else
static void* probe_thread(void* unused)
#endif
{
    (void)unused;
    unsigned round = 0;
    for (;;) {
#ifdef _WIN32
        Sleep(PROBE_PERIOD_MS);
        unsigned long long now = GetTickCount64();
#else
        sleep(PROBE_PERIOD_MS / 1000);
        unsigned long long now = 0;   /* cheap wall clock; cadence is the point */
#endif
        probe_dump(++round, now);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void rd_threadstate_probe_start(void)
{
    static int started = 0;
    if (started) return;
    if (!getenv("RD_PROBE_THREADSTATES")) return;
    started = 1;

#ifdef _WIN32
    HANDLE h = (HANDLE)_beginthreadex(NULL, 64 * 1024, probe_thread, NULL, 0, NULL);
    if (h) {
        fprintf(stderr, "[TSTATE] thread-state probe started (every %d ms)\n",
                PROBE_PERIOD_MS);
        CloseHandle(h);
    }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, probe_thread, NULL) == 0) {
        fprintf(stderr, "[TSTATE] thread-state probe started (every %d ms)\n",
                PROBE_PERIOD_MS);
        pthread_detach(th);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * Cross-thread host backtrace (Win64): suspend the guest thread's host
 * thread, grab CONTEXT, unwind with RtlVirtualUnwind, map every frame to the
 * guest function (ppu_prof_resolve_host) or the host symbol (dbghelp via
 * LoadLibrary: no build-system dependency). Only pointer/IP copies happen
 * while the thread is suspended; formatting and symbol lookup run after the
 * resume. This is the one part of the probe that touches the sampled thread
 * -- a 1-in-5s suspend of well under a millisecond, skipped entirely when
 * the env is unset.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <dbghelp.h>
typedef BOOL (WINAPI *SymInitFn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, char*);
static SymInitFn     s_sym_init;
static SymFromAddrFn s_sym_from_addr;
static int s_dbghelp_tried;

static void sym_init_once(void)
{
    if (s_dbghelp_tried) return;
    s_dbghelp_tried = 1;
    HMODULE m = LoadLibraryA("dbghelp.dll");
    if (!m) return;
    s_sym_init      = (SymInitFn)GetProcAddress(m, "SymInitialize");
    s_sym_from_addr = (SymFromAddrFn)GetProcAddress(m, "SymFromAddr");
    if (s_sym_init && !s_sym_init(GetCurrentProcess(), NULL, TRUE))
        s_sym_from_addr = NULL;
}

/* dbghelp contract: the buffer must start with a SYMBOL_INFO header whose
 * SizeOfStruct is filled in (88 bytes) and MaxNameLen names the room after
 * it. A bare char[128] with a zero header is the classic probe-boot AV:
 * SymFromAddr walks garbage internals. */
typedef struct { SYMBOL_INFO si; char name[128]; } SymBuf;

static void fmt_frame(void* rip, char* out, size_t n, size_t* pos)
{
    unsigned guest = ppu_prof_resolve_host(rip);
    if (guest) {
        *pos += (size_t)snprintf(out + *pos, n - *pos, "%s%08X", *pos ? "<-" : "", guest);
        return;
    }
    /* host (runtime) frame: symbol name when dbghelp cooperates, else RVA */
    SymBuf sb;
    memset(&sb, 0, sizeof(sb));
    sb.si.SizeOfStruct = sizeof(SYMBOL_INFO);
    sb.si.MaxNameLen   = sizeof(sb.name) - 1;
    DWORD64 disp = 0;
    if (s_sym_from_addr &&
        s_sym_from_addr(GetCurrentProcess(), (DWORD64)rip, &disp, (char*)&sb)
        && sb.si.Name[0]) {
        const char* s = sb.si.Name; if (!strncmp(s, "func_", 5)) s += 5;
        *pos += (size_t)snprintf(out + *pos, n - *pos, "%s%s", *pos ? "<-" : "", s);
    } else {
        HMODULE mod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &mod);
        *pos += (size_t)snprintf(out + *pos, n - *pos, "%shost+%llX",
                                 *pos ? "<-" : "",
                                 mod ? (unsigned long long)((char*)rip - (char*)mod)
                                     : (unsigned long long)(uintptr_t)rip);
    }
}

/* Suspend/copy/resume is inherently racy: a thread caught mid-startup or
 * teardown can hand out a context the unwinder faults on. The capture side
 * is __try-guarded so the WORST outcome is "(bt unavailable)" for one line
 * of one dump -- never a crash of the process under observation. */
static int capture_frames(HANDLE h, void** ips, int max)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &c)) return 0;
    CONTEXT u = c;
    int n = 0;
    for (int i = 0; i < max && u.Rip; i++) {
        ips[n++] = (void*)u.Rip;
        ULONG64 img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &img, NULL);
        if (!fe) break;   /* no unwind data for this IP: stop, don't guess */
        void* hd = NULL; ULONG64 estab = 0;
        /* ControlPc MUST be the current Rip (handler-data lookup indexes by
         * ControlPc - ImageBase); 0 made ntdll read wild offsets. */
        RtlVirtualUnwind(0, img, u.Rip, fe, &u, &hd, &estab, NULL);
    }
    return n;
}

static void probe_backtrace(unsigned tid, char* out, size_t outsz)
{
    size_t pos = 0;
    out[0] = 0;
    HANDLE h = (HANDLE)ppu_thread_host_handle(tid);
    if (!h) { snprintf(out, outsz, "(no host handle)"); return; }
    sym_init_once();
    if (SuspendThread(h) == (DWORD)-1) { snprintf(out, outsz, "(suspend failed)"); return; }
    void* ips[24]; int n = 0;
    __try {
        n = capture_frames(h, ips, 24);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    ResumeThread(h);
    if (!n) { snprintf(out, outsz, "(bt unavailable)"); return; }
    for (int i = 0; i < n && pos + 24 < outsz; i++) fmt_frame(ips[i], out, outsz, &pos);
    if (!pos) snprintf(out, outsz, "(no frames)");
}
#else
static void probe_backtrace(unsigned tid, char* out, size_t outsz)
{
    (void)tid; snprintf(out, outsz, "(win32-only)");
}
#endif
