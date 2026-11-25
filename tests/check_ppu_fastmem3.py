"""Checks the fast memory path of the PPU: the read accessors without a label
address, the alias answer of the gate, and the lean gate of PPU_FM4 1.

The lean gate reads one page byte at each access. The page byte holds the
route bits and the slow classes that step 3 tested at the call site (bounds
test, GCM ref-poll page, read sync).

The fixture includes the production header and the production arm text of
ppu_loader.cpp, and links the production runtime/syscalls/spu_ea_alias.c,
runtime/memory/guest_read_sync.c and runtime/spu/spu_coherency.c. It runs one
process per mode. Each mode covers each access class for reads and writes of
each width (8, 16, 32, 64 bits): plain page, null page, IDENTITY page, ALIAS
page, MIXED page, EA bit 31, raw SPU problem-state register, GCM ref-poll
address, out of bounds, and each diagnostic bit.

The check builds three paths in two layouts (PPU_FM_INLINE_FAST 1 and 0): the
lean gate (default), step 3 (PPU_FM4 0) and the path before step 3 (PPU_FM3
0). The same source must pass in each build. The object of the lean gate and
of step 3 must hold no out-of-line copy of an accessor: each lifted read is
inline. Then each planted mutant must fail.
"""
import argparse
import importlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = (ROOT / "vendor" / "ps3recomp").is_dir()      # the game tree; the fork test sits in the fork
if GAME:
    sys.path.insert(0, str(ROOT / "scripts"))
project = importlib.import_module("project") if GAME else None

VENDOR = ROOT / "vendor/ps3recomp" if GAME else ROOT
FASTMEM = Path("include/ps3emu/ppu_fastmem.h")
LOADER = Path("runtime/ppu/ppu_loader.cpp")
ALIAS = Path("runtime/syscalls/spu_ea_alias.c")
ARM_BEGIN = 'extern "C" volatile uint32_t ppu_fm_slow = PPU_FM_S_ALL;'
ARM_END = "is_fm = (knobs[i][0] == 'P'"
EXE = ".exe" if os.name == "nt" else ""

WINDOW = "uint32_t e = state == PPU_FM_ALIAS_ALIAS ? *ea - PPU_FM_ALIAS_DELTA : *ea;"
OUTSIDE = "    if (PPU_FM_UNLIKELY(off >= PPU_FM_ALIAS_HI - PPU_FM_ALIAS_LO)) return 0;"
GATE = "    (((flags) & (mask)) | (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind)))"
STATE = ("    return __atomic_load_n(&sysmem_spu_alias_page[(ea - PPU_FM_ALIAS_LO) "
         ">> PPU_FM_PAGE_SHIFT],")

# (name, anchor, replacement[, layouts]). The last group is the body of the
# path before this change; both paths share it.
HEADER_MUTANTS = [
    ("ALIAS page answered as IDENTITY", WINDOW, "uint32_t e = *ea;"),
    ("IDENTITY page answered as ALIAS", WINDOW,
     "uint32_t e = state != PPU_FM_ALIAS_ALIAS ? *ea - PPU_FM_ALIAS_DELTA : *ea;"),
    ("alias delta changed", WINDOW,
     "uint32_t e = state == PPU_FM_ALIAS_ALIAS ? *ea - 0x04000000u : *ea;"),
    ("MIXED page not sent out of line", "    if (state == PPU_FM_ALIAS_MIXED) return 0;\n", ""),
    ("a flag bit does not send a window page out of line", "    if (flags & mask) return 0;\n", ""),
    ("page outside the window answered inline", OUTSIDE,
     "    if (PPU_FM_UNLIKELY(off >= PPU_FM_ALIAS_HI - PPU_FM_ALIAS_LO)) return 1;"),
    ("bounds test of the alias answer removed",
     "    if (PPU_FM_UNLIKELY(ppu_fm_oob(e, n))) return 0;\n", ""),
    ("window lower bound at zero", "#define PPU_FM_ALIAS_LO       0x45000000u",
     "#define PPU_FM_ALIAS_LO       0x00000000u"),
    ("window covers the address space", OUTSIDE,
     "    if (PPU_FM_UNLIKELY(off >= 0xFFFFF000u)) return 0;"),
    ("upper bound past the raw SPU page", "#define PPU_FM_ALIAS_HI       0x50000000u",
     "#define PPU_FM_ALIAS_HI       0xFFFFFFFFu"),
    ("read32: ref-poll fence skipped",
     "    if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) ppu_fm_refpoll();\n", ""),
    ("gate ignores the page table", GATE, "    ((flags) & (mask))"),
    ("gate ignores the flag word", GATE,
     "    (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind))"),
    ("store body gets the address before the answer", "name##_body(a, ea, v);",
     "name##_body(a, (uint32_t)a, v);"),
    ("fire helper passes a null caller address", "    ppu_fm_hotread8(ea, PPU_FM_RA_OUT());\n",
     "    ppu_fm_hotread8(ea, (void*)0);\n"),
    ("fire dispatch ignores the caller address",
     "    do { if (ra) { out_fire; } else { inline_fire; } } while (0)",
     "    do { out_fire; } while (0)"),
    ("PPU_FM3 defaults to 0", "#  define PPU_FM3 1", "#  define PPU_FM3 0", (1,), "lean"),
    ("one copy per unit: null caller address", "#  define PPU_FM_RA3() PPU_FM_RA_OUT()",
     "#  define PPU_FM_RA3() ((void*)0)", (0,)),
    ("page state always from the first page", STATE,
     "    return __atomic_load_n(&sysmem_spu_alias_page[0],"),
    ("read accessor takes a label address again",
     "    return ppu_fm_read32_body(ea, PPU_FM_RA3());",
     "    PPU_FM_SITE return ppu_fm_read32_body(ea, PPU_FM_RA());"),
    ("byte order of read32 turned", "    return PPU_FM_BSWAP32(v);", "    return v;"),
    ("byte order of write16 turned", "    uint16_t b = (uint16_t)PPU_FM_BSWAP16(v);",
     "    uint16_t b = (uint16_t)(v);"),
    ("null test of read32 removed", "ea < 0x10000u))     return vm_read32(a);",
     "ea < 0x10000u)) { }"),
    ("raw SPU test of read32 removed",
     "if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) return vm_read32(a);",
     "if (PPU_FM_UNLIKELY(0)) return vm_read32(a);"),
]

ALIAS_MUTANTS = [
    ("ALIAS state published as MIXED",
     "    else if (prim == 2 && self == 0) state = SPU_ALIAS_PAGE_ALIAS;",
     "    else if (0) state = SPU_ALIAS_PAGE_ALIAS;"),
    ("page table not updated",
     "    atomic_store_explicit(&sysmem_spu_alias_page[page], state, "
     "memory_order_release);\n", ""),
    ("EA bit 31 rule lost", "    if ((ea & 0x80000000u) != 0u && ea < 0x90000000u)", "    if (0)"),
    ("partly covered page published as IDENTITY",
     "    if (self == 2 || prim == 0) state = SPU_ALIAS_PAGE_IDENTITY;",
     "    if (self == 2 || prim == 0 || self == 1) state = SPU_ALIAS_PAGE_IDENTITY;"),
]

ARM_MUTANTS = [
    ("alias pages not flagged", "        if (ppu_fm_in_alias(e))\n", "        if (0)\n"),
]

FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <stdint.h>
#include <unwind.h>
#endif
#include <map>
#include <string>
#include "ps3emu/ppu_fastmem.h"
#include "arm.inc"
#if PPU_FM4
/* The window rule of the lean path of the fixture model. 1: each page of
 * the window is flagged, so each access of the window goes out. 0: the
 * page byte holds the answer of the window (step 4, move 2). */
#define FIXTURE_WINDOW_OUT 0
#endif

/* ---- authored guest memory: a 4 GiB reserve. commit() backs 64 KB at an EA. */
#define GUEST_BYTES 0x100010000ull
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
uint8_t*  vm_base;
uint32_t  ppu_vm_size = 0;
uint32_t  ppu_hle_inject_base = 0x7FE00000u;
/* The coherency flag and bitmap, and the marks of the read sync, come from
   the production files runtime/spu/spu_coherency.c and
   runtime/memory/guest_read_sync.c. The fixture links both. */
static long long family[8], fires, refpolls, fixups, now_calls;
static uint64_t last_family_addr, fire_v;
static uint32_t fire_ea;
static void* fire_ra;
static std::map<std::string, std::string> environment;
static std::string fake_lines;
/* ---- authored clock and output of the PPU_FM_COUNT counter ---- */
uint64_t ppu_fm_count_now(void) { now_calls++; return 5; }
uint64_t ppu_fm_count_frequency(void) { return 1000000u; }
unsigned long ppu_fm_count_host_tid(void) { return 11; }
uint64_t ppu_fm_count_guest_tid(void) { return 0x1001u; }
void ppu_fm_count_emit(const char* line) { fake_lines += line; }

/* The stack at the fire, as return addresses. stack_at_fire[i] lies in the
 * function that called the i-th function below the reader. The header
 * functions on the stack can share one frame (a tail call), so the rule on
 * the fire address counts from the frame of the reader. */
static void* stack_at_fire[8];
#ifdef _WIN32
static void capture_stack(void) { CaptureStackBackTrace(0, 8, stack_at_fire, 0); }
#else
struct stack_out { void** frames; int n; };
static _Unwind_Reason_Code unwind_frame(struct _Unwind_Context* context, void* arg)
{
    stack_out* out = (stack_out*)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(context);
    if (!ip) return _URC_END_OF_STACK;
    /* The first two frames are unwind_frame and capture_stack, as the skip
     * of the Windows form. */
    if (out->n >= 2 && out->n < 10) out->frames[out->n - 2] = (void*)ip;
    out->n++;
    return out->n < 10 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static void capture_stack(void)
{
    stack_out out = { stack_at_fire, 0 };
    _Unwind_Backtrace(unwind_frame, &out);
}
#endif

extern "C" {
uint32_t alias_fixup_impl(uint32_t ea);
void sysmem_alias_record(uint32_t addr, uint32_t size);
/* The production fixup, behind a counter. The alias file is compiled with
 * sysmem_spu_alias_fixup=alias_fixup_impl. */
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { fixups++; return alias_fixup_impl(ea); }
/* Step 4, move 2: the writer of the page byte of a window page
 * (runtime/syscalls/spu_ea_alias.c, compiled with
 * ppu_fm_window_begin=fixture_window_begin and
 * ppu_fm_window_end=fixture_window_end). The end call records an error
 * when the page answer is not the new one, or when the route bits are not
 * set. The lean gate needs both: then each access of the page takes the
 * out-of-line function until the new answer is in the page byte. */
unsigned begin_calls, end_calls, window_errors;
void fixture_window_begin(unsigned page) { begin_calls++; ppu_fm_window_begin(page); }
void fixture_window_end(unsigned page, int state)
{
#if PPU_FM4
    /* A page there is an EA page: EA >> 12. */
    if ((page << 12) < 0x45000000u || (page << 12) >= 0x50000000u) window_errors |= 4;
    if (sysmem_spu_alias_page[page - 0x45000u] != (unsigned char)state) window_errors |= 1;
    if ((ppu_fm_kind[page] & (PPU_FM_K_READ | PPU_FM_K_WRITE)) != (PPU_FM_K_READ | PPU_FM_K_WRITE))
        window_errors |= 2;
#endif
    end_calls++;
    ppu_fm_window_end(page, state);
}
/* The handler of the read sync counts its calls. It runs for a read that
 * meets a marked page. The paths before PPU_FM4 mark no page in the modes
 * that do not say it, so it does not run there. */
static long long syncs;
static uint32_t sync_addr, sync_bytes;
void sync_handler(uint32_t address, uint32_t bytes, void*)
{
    syncs++; sync_addr = address; sync_bytes = bytes;
}
/* A hook of the read sync that records the state at its call. */
static unsigned hook_first, hook_last, hook_flag, hook_marked;
void order_hook(unsigned first, unsigned last)
{
    hook_first = first; hook_last = last;
    hook_flag = vm_read_sync_any; hook_marked = vm_read_sync_pages[first];
}
struct spu_context;                 /* the fixture has no SPU context */
void spu_coh_reserve(struct spu_context* ctx, uint32_t ea);
uint8_t  vm_read8 (uint64_t a) { family[0]++; last_family_addr = a; return 0x5Au; }
uint16_t vm_read16(uint64_t a) { family[1]++; last_family_addr = a; return 0x5AA5u; }
uint32_t vm_read32(uint64_t a) { family[2]++; last_family_addr = a; return 0x5AA5C35Au; }
uint64_t vm_read64(uint64_t a) { family[3]++; last_family_addr = a; return 0x5AA5C35A96967878ull; }
void vm_write8 (uint64_t a, uint8_t)  { family[4]++; last_family_addr = a; }
void vm_write16(uint64_t a, uint16_t) { family[5]++; last_family_addr = a; }
void vm_write32(uint64_t a, uint32_t) { family[6]++; last_family_addr = a; }
void vm_write64(uint64_t a, uint64_t) { family[7]++; last_family_addr = a; }
void ppu_fm_refpoll(void) { refpolls++; }
void ppu_fm_hotread8 (uint32_t ea, void* ra) { fires++; fire_ea = ea; fire_v = 0; fire_ra = ra; capture_stack(); }
void ppu_fm_hotread16(uint32_t ea, void* ra) { fires++; fire_ea = ea; fire_v = 0; fire_ra = ra; capture_stack(); }
void ppu_fm_hotread32(uint32_t ea, uint32_t v, void* ra) { fires++; fire_ea = ea; fire_v = v; fire_ra = ra; capture_stack(); }
void ppu_fm_hotread64(uint32_t ea, uint64_t v, void* ra) { fires++; fire_ea = ea; fire_v = v; fire_ra = ra; capture_stack(); }
}

char* getenv(const char* name)
{
    std::map<std::string, std::string>::const_iterator it = environment.find(name);
    return it == environment.end() ? 0 : const_cast<char*>(it->second.c_str());
}

static void reserve(void)
{
#ifdef _WIN32
    vm_base = (uint8_t*)VirtualAlloc(0, GUEST_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    if (!vm_base) { printf("reserve failed\n"); exit(3); }
#else
    vm_base = (uint8_t*)mmap(0, GUEST_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == (uint8_t*)-1) { printf("reserve failed\n"); exit(3); }
#endif
}

/* The byte of an EA in the model of the fixture. */
static unsigned char model(uint64_t ea) { return (unsigned char)((ea * 2654435761u) >> 13); }

/* Backs 64 KB at `base` and fills them with the model. */
static void commit(uint32_t base)
{
#ifdef _WIN32
    if (!VirtualAlloc(vm_base + base, 0x10000u, MEM_COMMIT, PAGE_READWRITE)) {
        printf("commit failed at %08X\n", base);
        exit(3);
    }
#else
    if (mprotect(vm_base + base, 0x10000u, PROT_READ | PROT_WRITE)) { printf("commit failed\n"); exit(3); }
#endif
    for (uint32_t i = 0; i < 0x10000u; i++) vm_base[base + i] = model(base + i);
}

static int in_window(uint32_t ea) { return ea >= 0x45000000u && ea < 0x50000000u; }

/* ---- the access classes ---- */
enum { K_PLAIN, K_BIT31, K_IDENT, K_ALIAS, K_MIXED_ALIAS, K_MIXED_SELF, K_NULL, K_RAW, K_REF,
       K_OOB, K_EDGE };
struct class_t { const char* name; int kind; uint32_t ea; };
/* K_ALIAS: the page is ALIAS, the access lands on EA - 0x05000000.
 * K_MIXED_ALIAS: the page is MIXED, the scan gives the primary.
 * K_MIXED_SELF: the page is MIXED (partly covered, with a covered primary), the scan gives the EA. */
static const class_t classes[] = {
    {"plain",        K_PLAIN,       0x10000100u},
    {"bit31",        K_BIT31,       0x82400100u},
    {"ident",        K_IDENT,       0x46000100u},
    {"ident0",       K_IDENT,       0x47000100u},
    {"alias",        K_ALIAS,       0x45300100u},
    {"mixed_alias",  K_MIXED_ALIAS, 0x45400100u},
    {"mixed_self",   K_MIXED_SELF,  0x46400100u},
    {"null",         K_NULL,        0x00000800u},
    {"raw",          K_RAW,         0xE0040400u},
    {"refpoll",      K_REF,         0x7FE02008u},
    {"oob",          K_OOB,         0x49000100u},
    {"oob_edge",     K_EDGE,        0x47FFFFFFu},
    {"end4",         K_EDGE,        0x47FFFFFCu},   /* 8 bytes end past the end: only width 8 */
    {"end8",         K_EDGE,        0x47FFFFF8u},   /* 8 bytes end inside the page before the end */
    {"end_prev",     K_EDGE,        0x47FFEFFCu},   /* the page before the page of the end */
};
#define CLASSES (int)(sizeof classes / sizeof classes[0])
static const int WIDTH[8] = { 1, 2, 4, 8, 1, 2, 4, 8 };
static const int OFFSET[8] = { 0, 2, 4, 8, 0x20, 0x22, 0x24, 0x28 };  /* reads, then stores */
static const uint64_t MAGIC[8] = { 0x5Au, 0x5AA5u, 0x5AA5C35Au, 0x5AA5C35A96967878ull,
                                   0, 0, 0, 0 };

static int fail(const char* what, unsigned long long value, unsigned long long expected)
{
    printf("FAIL %s=%llx expected=%llx\n", what, value, expected);
    return 1;
}

/* No knob in the environment, the bounds off, then the production arm, the
 * alias records of the fixture and the pages of the classes. */
static void reset(void)
{
    memset(family, 0, sizeof family);
    fires = refpolls = fixups = syncs = 0;
    last_family_addr = 0;
    ppu_vm_size = 0; g_spu_coh_armed = 0; ppu_fm_spin_det = 1;
    memset(g_spu_coh_bitmap, 0, PPU_FM_COH_BITMAP_BYTES);
    fake_lines.clear();
    environment.clear();
    ppu_fm_arm();
    sysmem_alias_record(0x46000000u, 0x2000u);          /* two IDENTITY pages */
    sysmem_alias_record(0x40300000u, 0x1000u);          /* page 0x45300000 is ALIAS */
    sysmem_alias_record(0x40400000u, 0x800u);           /* page 0x45400000 is MIXED */
    sysmem_alias_record(0x41400000u, 0x1000u);          /* primary of the next record */
    sysmem_alias_record(0x46400000u, 0x800u);           /* page 0x46400000 is MIXED */
    commit(0x48000000u);             /* the read of 8 bytes of the edge class ends here */
    commit(0x43000000u);             /* as its twin */
    for (int c = 0; c < CLASSES; c++) {
        int exact = classes[c].kind == K_OOB || classes[c].kind == K_EDGE || classes[c].kind == K_REF;
        uint32_t ea = classes[c].ea + (exact ? 0 : OFFSET[7]);
        commit(ea & ~0xFFFFu);
        if (in_window(ea)) commit((ea - 0x05000000u) & ~0xFFFFu);   /* the primary page */
    }
}

/* True when the out-of-line function gives the access to the family. */
static int out_family(int acc, uint32_t ea)
{
    int read = acc < 4;
    if (ppu_fm_slow & (read ? PPU_FM_S_READ : PPU_FM_S_WRITE)) return 1;
    if (ppu_fm_oob(ea, (uint32_t)WIDTH[acc])) return 1;
    if (ea < 0x10000u && acc != 1) return 1;            /* null-read report; read16 has no test */
    if ((acc == 2 || acc == 6) && spu_raw_is_reg(ea)) return 1;   /* raw SPU register */
    return 0;
}

#if PPU_FM4
/* The lean gate tests one page byte. Bounds rule: a page is flagged when an
 * access of 8 bytes that starts at its last byte ends past ppu_vm_size. So
 * the page before the end is flagged too, for each width. */
static int page_flagged(uint32_t ea)
{
    return ppu_vm_size != 0
        && (uint64_t)(ea & ~(uint32_t)0xFFFu) + PPU_FM_PAGE_SIZE + 7u > ppu_vm_size;
}
#endif

/* Step 4, move 2: the arm rule for the page of an EA
 * (runtime/ppu/ppu_loader.cpp, ppu_fm_page_kind), as the fixture computes
 * it. It does not read the page table. */
static int arm_rule(uint32_t ea)
{
    const uint32_t page = ea & ~(uint32_t)0xFFFu;
    const uint32_t refpoll = (ppu_hle_inject_base + 0x2008u) & ~(PPU_FM_PAGE_SIZE - 1u);
    return (ppu_vm_size != 0 && (uint64_t)page + PPU_FM_PAGE_SIZE + 7u > ppu_vm_size)
        || ea < 0x10000u || spu_raw_is_reg(ea) || page == refpoll;
}

/* True when the access leaves the gate for the out-of-line function. */
static int goes_out(int acc, const class_t* c)
{
    int read = acc < 4;
    int oob = ppu_vm_size != 0 && ppu_fm_oob(c->ea, (uint32_t)WIDTH[acc]);
    if (ppu_fm_slow & (read ? PPU_FM_S_READ : PPU_FM_S_WRITE)) return 1;
#if PPU_FM4
    (void)oob;
#endif
    switch (c->kind) {
    case K_PLAIN:
    case K_BIT31:
#if PPU_FM4
        return page_flagged(c->ea);
#else
        return oob;
#endif
    case K_NULL:
    case K_RAW: return 1;                               /* the page table flags them */
    case K_REF: return read || oob;                     /* the page table flags it for reads */
    case K_OOB:
    case K_EDGE:
#if PPU_FM4
        return page_flagged(c->ea) || (in_window(c->ea) && FIXTURE_WINDOW_OUT);
#else
        return oob || (!PPU_FM3 && in_window(c->ea));
#endif
    default:
        /* Pages of the window. The page byte holds the answer of the
         * window (move 2). MIXED: out of line. ALIAS: inline on the
         * primary, unless the arm rule flags the page of the primary
         * address. IDENTITY: out of line when the arm rule flags the
         * page. */
#if PPU_FM4
        if (FIXTURE_WINDOW_OUT) return 1;              /* FIXTURE_WINDOW_RULE */
        if (c->kind == K_MIXED_ALIAS || c->kind == K_MIXED_SELF) return 1;
        if (c->kind == K_ALIAS) return arm_rule(c->ea - 0x05000000u);
        return arm_rule(c->ea);
#else
        if (!PPU_FM3) return 1;
        if (c->kind == K_MIXED_ALIAS || c->kind == K_MIXED_SELF) return 1;
        return ppu_fm_oob(c->ea, (uint32_t)WIDTH[acc]);
#endif
    }
}

static uint64_t read_value(int acc, uint32_t ea)
{
    switch (acc) {
    case 0: return ppu_fm_read8(ea);
    case 1: return ppu_fm_read16(ea);
    case 2: return ppu_fm_read32(ea);
    default: return ppu_fm_read64(ea);
    }
}

static void write_value(int acc, uint32_t ea, uint64_t v)
{
    switch (acc) {
    case 4: ppu_fm_write8(ea, (uint8_t)v); break;
    case 5: ppu_fm_write16(ea, (uint16_t)v); break;
    case 6: ppu_fm_write32(ea, (uint32_t)v); break;
    default: ppu_fm_write64(ea, v); break;
    }
}

/* The bytes of the model at an EA, in guest byte order. */
static uint64_t word_at(uint32_t ea, int width)
{
    uint64_t v = 0;
    for (int b = 0; b < width; b++) v = (v << 8) | vm_base[ea + b];
    return v;
}

static void refill(uint32_t ea, int width)
{
    for (int b = 0; b < width; b++) vm_base[ea + b] = model(ea + b);
}

static long long family_total(void)
{
    long long n = 0;
    for (int i = 0; i < 8; i++) n += family[i];
    return n;
}

static uint64_t word_at_model(uint32_t ea, int width);

/* One access of accessor `acc` (read8 .. write64) on one class. */
static int probe(int acc, const class_t* c)
{
    const char* names[8] = { "read8", "read16", "read32", "read64", "write8", "write16",
                             "write32", "write64" };
    char what[64];
    const int width = WIDTH[acc], read = acc < 4;
    const int exact = c->kind == K_OOB || c->kind == K_EDGE || c->kind == K_REF;
    uint32_t ea = c->ea + (exact ? 0 : OFFSET[acc]);
    uint32_t eff = (c->kind == K_ALIAS || c->kind == K_MIXED_ALIAS) ? ea - 0x05000000u : ea;
    uint32_t twin = in_window(ea) ? ea - 0x05000000u : ea;
    int to_family = goes_out(acc, c) && out_family(acc, ea);
    int fence = c->kind == K_REF && acc == 2 && !to_family;
    long long fix_want = in_window(ea) && goes_out(acc, c);
    memset(family, 0, sizeof family);
    fixups = refpolls = fires = 0;
    refill(ea, width);
    if (twin != ea) refill(twin, width);
    snprintf(what, sizeof what, "%s.%s", c->name, names[acc]);
    if (read) {
        uint64_t want = to_family ? MAGIC[acc] : word_at(eff, width);
        uint64_t got = read_value(acc, ea);
        if (got != want) return fail(what, got, want);
    } else {
        uint64_t pattern = 0x1122334455667788ull >> (64 - 8 * width);
        uint64_t before = word_at(eff, width);
        write_value(acc, ea, pattern);
        uint64_t got = word_at(eff, width);
        if (to_family) {
            if (got != before) return fail(what, got, before);
            if (family[acc] && last_family_addr != ea) return fail(what, last_family_addr, ea);
        } else {
            if (got != pattern) return fail(what, got, pattern);
        }
        if (twin != eff && word_at(twin, width) != word_at_model(twin, width))
            return fail(what, word_at(twin, width), word_at_model(twin, width));
        refill(eff, width);
    }
    if (family[acc] != to_family) return fail(what, family[acc], to_family);
    if (family_total() != to_family) return fail(what, family_total(), to_family);
    if (fixups != fix_want) return fail(what, fixups, fix_want);
    if (refpolls != fence) return fail(what, refpolls, fence);
    if (fires) return fail(what, fires, 0);
    return 0;
}
''' + r'''
/* The model at an EA, in guest byte order, as a word. */
static uint64_t word_at_model(uint32_t ea, int width)
{
    uint64_t v = 0;
    for (int b = 0; b < width; b++) v = (v << 8) | model(ea + b);
    return v;
}

/* Each class, each accessor. */
static int probes(void)
{
    for (int c = 0; c < CLASSES; c++)
        for (int acc = 0; acc < 8; acc++)
            if (probe(acc, &classes[c])) return 1;
    return 0;
}

/* The page answer of the table and the production fixup, by direct calls. */
static int alias_probes(void)
{
    static const struct { uint32_t ea, want; } fix[] = {
        { 0x80440440u, 0x40440440u },    /* EA bit 31: main storage at 0x40000000 | EA */
        { 0x8FFFFFFFu, 0x4FFFFFFFu },
        { 0x90000000u, 0x90000000u },    /* kernel range: no rule */
        { 0xE0040400u, 0xE0040400u },    /* raw SPU: bit 31 keeps its meaning */
        { 0x85000008u, 0x45000008u },    /* the rule comes before the window: an EA inside the window */
        { 0x44FFFFFFu, 0x44FFFFFFu },
        { 0x45300100u, 0x40300100u },    /* ALIAS page */
        { 0x46000100u, 0x46000100u },    /* IDENTITY page */
        { 0x45400100u, 0x40400100u },    /* MIXED page: the scan gives the primary */
        { 0x45400800u, 0x45400800u },
        { 0x46400100u, 0x46400100u },
        { 0x46400800u, 0x41400800u },    /* MIXED page: the part outside the record gives the primary */
        { 0x50000000u, 0x50000000u },
        { 0xFFFFFFFFu, 0xFFFFFFFFu },
    };
    /* The public alias table is part of the PPU_FM3 path only. */
#if PPU_FM3
    static const struct { uint32_t page; unsigned char want; } state[] = {
        { 0x46000000u, PPU_FM_ALIAS_IDENTITY }, { 0x46001000u, PPU_FM_ALIAS_IDENTITY },
        { 0x47000000u, PPU_FM_ALIAS_IDENTITY },
        { 0x45300000u, PPU_FM_ALIAS_ALIAS },
        { 0x45400000u, PPU_FM_ALIAS_MIXED }, { 0x46400000u, PPU_FM_ALIAS_MIXED },
    };
    for (unsigned i = 0; i < sizeof state / sizeof state[0]; i++) {
        unsigned char got = sysmem_spu_alias_page[(state[i].page - PPU_FM_ALIAS_LO) >> PPU_FM_PAGE_SHIFT];
        if (got != state[i].want) return fail("page_state", got, state[i].want);
    }
#endif
    for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; i++) {
        uint32_t got = sysmem_spu_alias_fixup(fix[i].ea);
        if (got != fix[i].want) return fail("fixup", got, fix[i].want);
    }
#if PPU_FM3
    if (sizeof sysmem_spu_alias_page != 0xB000u) return fail("alias_pages", sizeof sysmem_spu_alias_page, 0xB000u);
#endif
    return 0;
}

/* Before ppu_fm_arm: each bit of the flag word is set. */
static int unarmed(void)
{
    memset(family, 0, sizeof family);
    if (ppu_fm_slow != PPU_FM_S_ALL) return fail("initial_flag_word", ppu_fm_slow, PPU_FM_S_ALL);
    reserve();
    commit(0x10000000u);
    refill(0x10000100u, 8);
    if (read_value(2, 0x10000100u) != MAGIC[2]) return fail("unarmed_read", read_value(2, 0x10000100u), MAGIC[2]);
    write_value(6, 0x10000100u, 1);
    if (family[2] != 1 || family[6] != 1) return fail("unarmed_family", family[2] + family[6], 2);
    return 0;
}

/* The spin detector: 200000 reads of one address, then one fire. The fire
 * gets the EA of the body, the guest word and a caller address in the
 * lifted function (here: in the function that reads). */
#define SPIN(width)                                                                    \
extern "C" FIXTURE_NOINLINE FIXTURE_OPTNONE uint64_t spin_read##width(uint32_t ea)                    \
{                                                                                      \
    uint64_t last = 0;                                                                 \
    for (int i = 0; i < 200000; i++) last = ppu_fm_read##width(ea);                   \
    return last;                                                                       \
}
#if defined(_MSC_VER) && !defined(__clang__)
#define FIXTURE_NOINLINE __declspec(noinline)
#else
#define FIXTURE_NOINLINE __attribute__((noinline))
#endif
/* optnone keeps each reader function in one block: the compiler moves no
 * part of it to a cold section. The range of the function then covers
 * each call site in it. */
#if defined(__clang__)
#define FIXTURE_OPTNONE __attribute__((optnone))
#else
#define FIXTURE_OPTNONE
#endif
SPIN(8) SPIN(16) SPIN(32) SPIN(64)

/* The start of the function that holds an address. The rule on the fire
 * address uses it, so that it does not depend on the order of the functions
 * in the image. */
#ifdef _WIN32
static void* enclosing(void* pc)
{
    typedef const unsigned int* (WINAPI *LOOKUP)(ULONG_PTR, ULONG_PTR*, void*);
    static LOOKUP lookup = (LOOKUP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlLookupFunctionEntry");
    ULONG_PTR base = 0;
    const unsigned int* entry = lookup ? lookup((ULONG_PTR)pc, &base, 0) : 0;
    return entry ? (void*)(base + entry[0]) : 0;
}
#else
static void* enclosing(void* pc) { return _Unwind_FindEnclosingFunction(pc); }
#endif

static int spin_at(int acc, uint32_t ea, uint32_t fire_ea_want)
{
    const char* names[4] = { "read8", "read16", "read32", "read64" };
    char what[64];
    uint64_t (*fn[4])(uint32_t) = { spin_read8, spin_read16, spin_read32, spin_read64 };
    uint64_t raw[4] = { 0, 0, vm_base[fire_ea_want] | vm_base[fire_ea_want + 1] << 8
                        | vm_base[fire_ea_want + 2] << 16 | (uint64_t)vm_base[fire_ea_want + 3] << 24,
                        (uint64_t)vm_base[fire_ea_want] | (uint64_t)vm_base[fire_ea_want + 1] << 8
                        | (uint64_t)vm_base[fire_ea_want + 2] << 16 | (uint64_t)vm_base[fire_ea_want + 3] << 24
                        | (uint64_t)vm_base[fire_ea_want + 4] << 32 | (uint64_t)vm_base[fire_ea_want + 5] << 40
                        | (uint64_t)vm_base[fire_ea_want + 6] << 48 | (uint64_t)vm_base[fire_ea_want + 7] << 56 };
    void* start[4] = { (void*)spin_read8, (void*)spin_read16, (void*)spin_read32, (void*)spin_read64 };
    snprintf(what, sizeof what, "spin_%s", names[acc]);
    /* The first read of an address only records it. Reads after it count
     * (PPU_FM_SPIN_DET): one read comes first. */
    (void)read_value(acc, ea);
    fires = 0; fire_ra = 0;
    (void)fn[acc](ea);
    if (fires != 1) return fail(what, fires, 1);
    if (fire_ea != fire_ea_want) return fail(what, fire_ea, fire_ea_want);
    if (fire_v != raw[acc]) return fail(what, fire_v, raw[acc]);
    /* On the lean gate, an access of a page with a route bit takes the
     * out-of-line function. The report then comes from that function or
     * from its tail call: the reported function is one of the functions
     * from the stub down to the reader (inclusive), and at least one
     * function lies between the stub and the reader. */
    int out = PPU_FM4 && (ppu_fm_kind[ea >> PPU_FM_PAGE_SHIFT] & (PPU_FM_K_READ | PPU_FM_K_WRITE));
    /* The fire address of an inline access lies in the reader. Inline
     * layout: the reader holds the call site (always_inline bodies are
     * inlined into an optnone function), and the header function that
     * reports reads its own return address. One copy per unit: the
     * out-of-line accessor passes its own return address, which lies in
     * the reader that called it. The stack at the fire proves the layout
     * of the one copy per unit: at least one function (the accessor copy)
     * lies between the reader and the function that reports. Header
     * functions that only call the next one can share one frame, so the
     * check does not count frames below that. */
    int frame = 0;
    while (frame < 8 && enclosing(stack_at_fire[frame]) != start[acc]) frame++;
    void* found = enclosing(fire_ra);
    int site_ok;
    if (out) {
        int bounded = frame >= 1 && frame < 8;
        site_ok = 0;
        for (int i = 0; bounded && i <= frame; i++)
            site_ok = site_ok || enclosing(stack_at_fire[i]) == found;
    } else if (PPU_FM3) {
        site_ok = found == start[acc] && (PPU_FM_INLINE_FAST || frame >= 1);
    } else {
        /* The build before step 3. The report address is a label of the
         * accessor, and the accessor can stay one out-of-line function next
         * to its reader. So the rule is the rule of that build: the address
         * lies in the 4096 bytes of the reader, and with one copy per unit
         * not in that copy. */
        site_ok = (uintptr_t)fire_ra - (uintptr_t)start[acc] < 0x1000;
#if !PPU_FM_INLINE_FAST
        void* copies[4] = { (void*)ppu_fm_read8, (void*)ppu_fm_read16, (void*)ppu_fm_read32,
                           (void*)ppu_fm_read64 };
        if ((uintptr_t)fire_ra - (uintptr_t)copies[acc] < 0x400) site_ok = 0;
#endif
    }
    if (!site_ok) {
        printf("stack at fire (reader %p, frame %d):", start[acc], frame);
        for (int i = 0; i < 8; i++) printf(" %p/%p", stack_at_fire[i], enclosing(stack_at_fire[i]));
        printf(", fire address %p\n", fire_ra);
        return fail(what, (unsigned long long)(uintptr_t)found,
                    out ? 0 : (unsigned long long)(uintptr_t)start[acc]);
    }
    (void)fn[acc](ea);
    if (fires != 2) return fail(what, fires, 2);
    return 0;
}

static int spin_mode(void)
{
    /* A plain page, an IDENTITY page, an ALIAS page (the EA of the body is the primary). */
    static const uint32_t eas[3][2] = { { 0x10000100u, 0x10000100u }, { 0x46000100u, 0x46000100u },
                                        { 0x45300100u, 0x40300100u } };
    for (int c = 0; c < 3; c++)
        for (int acc = 0; acc < 4; acc++)
            if (spin_at(acc, eas[c][0], eas[c][1])) return 1;
    return 0;
}

static int spin_off(void)
{
    for (int acc = 0; acc < 4; acc++) {
        char what[32];
        snprintf(what, sizeof what, "spin_off_%d", acc);
        fires = 0;
        for (int i = 0; i < 400000; i++) (void)read_value(acc, 0x10000100u);
        if (fires) return fail(what, fires, 0);
    }
    return 0;
}

/* With the count bit set, each access leaves the fast path and takes the
 * out-of-line function, on a page of the window and on a plain page (the
 * comment of ppu_fm_arm in the loader). The counter reads the clock once
 * per 16384 hits. */
static int counter(void)
{
    uint32_t base = 0x46000100u;
    now_calls = 0;
    for (int i = 0; i < 16384; i++) (void)ppu_fm_read32(base + 4u);
    if (now_calls != 1) return fail("count_read32_window", now_calls, 1);
    for (int i = 0; i < 16384; i++) {
        (void)ppu_fm_read8(base);
        (void)ppu_fm_read16(base + 2u);
        (void)ppu_fm_read64(base + 8u);
        ppu_fm_write8(base + 0x20u, (uint8_t)i);
        ppu_fm_write16(base + 0x22u, (uint16_t)i);
        ppu_fm_write32(base + 0x24u, (uint32_t)i);
        ppu_fm_write64(base + 0x28u, (uint64_t)i);
    }
    if (now_calls != 8) return fail("count_window", now_calls, 8);
    base = 0x10000100u;
    for (int i = 0; i < 16384; i++) {
        (void)ppu_fm_read8(base);
        (void)ppu_fm_read16(base + 2u);
        (void)ppu_fm_read32(base + 4u);
        (void)ppu_fm_read64(base + 8u);
        ppu_fm_write8(base + 0x20u, (uint8_t)i);
        ppu_fm_write16(base + 0x22u, (uint16_t)i);
        ppu_fm_write32(base + 0x24u, (uint32_t)i);
        ppu_fm_write64(base + 0x28u, (uint64_t)i);
    }
    if (now_calls != 16) return fail("count_plain", now_calls, 16);
    return 0;
}

/* Reserves the line of an EA as the SPU does (the production writer), or
 * clears the line of the fixture. Production never
 * clears a bit. */
static void set_line(uint32_t ea, int on)
{
    if (on) { spu_coh_reserve(0, ea); return; }
    uint32_t line = ea >> 7;
    g_spu_coh_bitmap[line >> 3] &= (unsigned char)~(1u << (line & 7));
}

/* A store on a reserved line goes to the family with the address of the
 * access (the alias address on an ALIAS page), not to memory. */
static int coherent(void)
{
    static const uint32_t alias = 0x45300020u, primary = 0x40300020u, plain = 0x10000020u;
    for (int acc = 4; acc < 8; acc++) {
        uint32_t v = (uint32_t)acc;
        memset(family, 0, sizeof family);
        last_family_addr = 0;
        set_line(primary, 1);
        write_value(acc, alias, v);
        set_line(primary, 0);
        if (family[acc] != 1 || last_family_addr != alias) return fail("coh_alias", family[acc], 1);
        set_line(alias, 1);                         /* the alias line itself: not reserved */
        write_value(acc, alias, v);
        set_line(alias, 0);
        if (family[acc] != 1) return fail("coh_alias_line", family[acc], 1);
        memset(family, 0, sizeof family);
        set_line(plain, 1);
        write_value(acc, plain, v);
        set_line(plain, 0);
        if (family[acc] != 1 || last_family_addr != plain) return fail("coh_plain", family[acc], 1);
    }
    return 0;
}

/* ---- PPU_FM4: the lean gate ---- */
#if PPU_FM4
/* Each bit of the page byte: the content that the writers must set. The
 * rules of the boot arm, of the read sync, and of the bounds. */
static int page_byte(const char* what, uint32_t ea, unsigned char want)
{
    unsigned char got = ppu_fm_kind[ea >> PPU_FM_PAGE_SHIFT];
    if (got != want) return fail(what, got, want);
    return 0;
}

static int lean_bits(void)
{
    /* The boot arm: no knob, bounds off. */
    if (page_byte("bits_plain", 0x10000100u, 0)) return 1;
    if (page_byte("bits_null0", 0x00000000u, PPU_FM_K_READ | PPU_FM_K_WRITE)) return 1;
    if (page_byte("bits_null1", 0x00001000u, PPU_FM_K_READ)) return 1;
    if (page_byte("bits_raw", 0xE0040400u, PPU_FM_K_READ | PPU_FM_K_WRITE)) return 1;
    if (page_byte("bits_refpoll", 0x7FE02008u, PPU_FM_K_READ)) return 1;
    /* The read sync: the pages of the range get the read bit and the owner
     * bit. The page before the range gets the read bit. No page gets the
     * write bit. */
    if (vm_read_sync_mark_range(0x20001000u, 0x2000u)) return fail("sync_mark", 1, 0);
    if (page_byte("bits_sync_before", 0x20000000u, PPU_FM_K_READ)) return 1;
    if (page_byte("bits_sync_first", 0x20001000u, PPU_FM_K_READ | PPU_FM_K_SYNC)) return 1;
    if (page_byte("bits_sync_last", 0x20002000u, PPU_FM_K_READ | PPU_FM_K_SYNC)) return 1;
    if (page_byte("bits_sync_after", 0x20003000u, 0)) return 1;
    /* The bounds: the page of the end and the page before it are flagged
     * for reads and writes. The page before that is not flagged. */
    ppu_vm_size = 0x10010000u;
    ppu_fm_arm();
    if (page_byte("bits_end", 0x10010000u, PPU_FM_K_READ | PPU_FM_K_WRITE)) return 1;
    if (page_byte("bits_end_minus1", 0x1000F000u, PPU_FM_K_READ | PPU_FM_K_WRITE)) return 1;
    if (page_byte("bits_end_minus2", 0x1000E000u, 0)) return 1;
    return 0;
}

/* The read sync on the lean path. The page byte sends each read of a marked
 * page, and each read that ends in a marked page, to the out-of-line
 * function. That function has the test of the marks: it calls the handler
 * for that read, with the address and the width of the access. No store
 * calls it. */
static int lean_sync(void)
{
    /* Marked: [base + 0x1000, base + 0x3000). */
    const uint32_t base = 0x21000000u;
    const char* names[8] = { "read8", "read16", "read32", "read64", "write8", "write16",
                             "write32", "write64" };
    char what[64];
    commit(base);
    vm_read_sync_set_handler(sync_handler, 0);
    /* Before the mark no page is flagged: no read calls the handler. */
    syncs = 0;
    for (int acc = 0; acc < 4; acc++) (void)read_value(acc, base + 0x1004u);
    if (syncs) return fail("sync_unmarked", syncs, 0);
    if (vm_read_sync_mark_range(base + 0x1000u, 0x2000u)) return fail("sync_mark", 1, 0);
    for (int acc = 0; acc < 8; acc++) {
        const int width = WIDTH[acc], read = acc < 4;
        const uint32_t at[5] = { base + 0x1004u,        /* inside the range */
                                 base + 0x1000u - width,     /* the page before: ends before the range */
                                 base + 0x1000u - width + 1, /* the page before: its last byte is in the range */
                                 base + 0x2FFCu,             /* the last marked page */
                                 base + 0x3000u };           /* the page after the range */
        const int want[5] = { read, 0, read, read, 0 };
        for (int i = 0; i < 5; i++) {
            snprintf(what, sizeof what, "sync_%s_%d", names[acc], i);
            syncs = 0; sync_addr = 0; sync_bytes = 0;
            if (read) (void)read_value(acc, at[i]);
            else write_value(acc, at[i], (uint64_t)acc);
            if (syncs != want[i]) return fail(what, syncs, want[i]);
            if (want[i] && (sync_addr != at[i] || sync_bytes != (uint32_t)width))
                return fail(what, sync_addr, at[i]);
        }
    }
    /* The writer calls the hook after its marks and after its flag. A hook
     * of the fixture records the state at the call. The pages of this
     * range stay without a page byte: no later test reads them. */
    vm_read_sync_set_mark_hook(order_hook);
    hook_flag = hook_marked = 0;
    if (vm_read_sync_mark_range(base + 0x10000u, 0x1000u)) return fail("sync_mark_hook", 1, 0);
    if (hook_first != (base + 0x10000u) >> 12 || hook_last != hook_first)
        return fail("sync_hook_pages", hook_first, (base + 0x10000u) >> 12);
    if (hook_flag != 1) return fail("sync_hook_flag", hook_flag, 1);
    if (hook_marked != 1) return fail("sync_hook_marked", hook_marked, 1);
    return 0;
}
/* Step 4, move 2: the alias window answer of the call site. The page byte
 * of a window page holds the answer: the alias bit, the route bits, or
 * no bit. */
static int lean_alias(void)
{
    static const uint32_t alias = 0x45300100u;       /* ALIAS: the primary is 0x40300100 */
    static const uint32_t page = 0x45300000u;
    const unsigned char route = (unsigned char)(PPU_FM_K_READ | PPU_FM_K_WRITE);
    unsigned char got;
    /* After the records of the fixture: ALIAS has the alias bit and no
     * route bit. MIXED has the route bits. IDENTITY has no bit. The
     * primary pages have no bit. */
    if (page_byte("win_alias", alias, PPU_FM_K_ALIAS)) return 1;
    if (page_byte("win_ident", 0x46000100u, 0)) return 1;
    if (page_byte("win_ident0", 0x47000100u, 0)) return 1;
    if (page_byte("win_mixed", 0x45400100u, route)) return 1;
    if (page_byte("win_mixed_self", 0x46400100u, route)) return 1;
    if (page_byte("primary_alias", 0x40300100u, 0)) return 1;
    /* The writer: begin, the page answer, end. The wrappers of the fixture
     * record an error when the order is not that one, or when the page is
     * not an EA page of the window. The page 0x47000000 goes ALIAS, MIXED,
     * IDENTITY. A record inside the window changes two pages: its own page
     * and the image of it. */
    begin_calls = end_calls = window_errors = 0;
    sysmem_alias_record(0x42000000u, 0x1000u);
    if (begin_calls != 1 || end_calls != 1) return fail("win_publish_calls", begin_calls * 10u + end_calls, 11u);
    if (window_errors) return fail("win_publish_order", window_errors, 0);
    if (page_byte("win_to_alias", 0x47000100u, PPU_FM_K_ALIAS)) return 1;
    begin_calls = end_calls = window_errors = 0;
    sysmem_alias_record(0x47000800u, 0x800u);
    if (begin_calls != 2 || end_calls != 2) return fail("win_publish_mixed_calls", begin_calls * 10u + end_calls, 22u);
    if (window_errors) return fail("win_publish_mixed_order", window_errors, 0);
    if (page_byte("win_to_mixed", 0x47000100u, route)) return 1;
    begin_calls = end_calls = window_errors = 0;
    sysmem_alias_record(0x47000000u, 0x1000u);
    if (begin_calls != 2 || end_calls != 2) return fail("win_publish_ident_calls", begin_calls * 10u + end_calls, 22u);
    if (window_errors) return fail("win_publish_ident_order", window_errors, 0);
    if (page_byte("win_to_ident", 0x47000100u, 0)) return 1;
    /* The page byte of the primary address decides too. A second ALIAS
     * page: 0x45500000, primary 0x40500000. The read sync flags the
     * primary page for reads. A read through the alias goes out of line,
     * and its handler runs with the primary address. A store keeps the
     * inline answer and lands on the primary. */
    commit(0x40500000u);
    sysmem_alias_record(0x40500000u, 0x1000u);
    if (page_byte("win2_alias", 0x45500100u, PPU_FM_K_ALIAS)) return 1;
    vm_read_sync_set_handler(sync_handler, 0);
    if (vm_read_sync_mark_range(0x40500000u, 0x1000u)) return fail("win_sync_mark", 1, 0);
    if (page_byte("win2_primary", 0x40500100u, PPU_FM_K_READ | PPU_FM_K_SYNC)) return 1;
    if (page_byte("win2_alias_marked", 0x45500100u, PPU_FM_K_ALIAS)) return 1;
    syncs = 0; sync_addr = sync_bytes = 0;
    fixups = 0;
    (void)ppu_fm_read32(0x45500100u);
    if (fixups != 1 || syncs != 1 || sync_addr != 0x40500100u || sync_bytes != 4)
        return fail("win_alias_sync_read", fixups * 1000000ull + syncs * 1000ull + sync_bytes, 1001004ull);
    fixups = 0; syncs = 0;
    ppu_fm_write32(0x45500100u, 0x11223344u);
    if (fixups || syncs) return fail("win_alias_sync_write", fixups + syncs, 0);
    if (word_at(0x40500100u, 4) != 0x11223344u) return fail("win_alias_store_landed", word_at(0x40500100u, 4), 0x11223344u);
    /* An ALIAS page whose primary page is flagged by the arm rule. The
     * access goes out of line with its own address. The out-of-line
     * function answers the alias: an access in bounds gets the fast body
     * there, an access out of bounds goes to the family with the address
     * of the access. The window page keeps its alias bit. The arm sets
     * bits and clears none, so this part runs after the part above. */
    ppu_vm_size = 0x40300800u;
    ppu_fm_arm();
    if (page_byte("win_alias_bounds", alias, PPU_FM_K_ALIAS)) return 1;
    if (page_byte("primary_bounds", 0x40300100u, route)) return 1;
    for (int acc = 0; acc < 8; acc++) {
        memset(family, 0, sizeof family);
        last_family_addr = 0;
        fixups = 0;
        if (acc < 4) (void)read_value(acc, alias);
        else write_value(acc, alias, (uint64_t)acc);
        if (fixups != 1) return fail("win_alias_bounds_fixups", fixups, 1);
        if (family[acc]) return fail("win_alias_bounds_family", family[acc], 0);
        memset(family, 0, sizeof family);
        last_family_addr = 0;
        fixups = 0;
        /* The primary is 0x40300800: each width ends past ppu_vm_size. */
        const uint32_t oob = 0x45300800u;
        if (acc < 4) (void)read_value(acc, oob);
        else write_value(acc, oob, (uint64_t)acc);
        if (fixups != 1) return fail("win_oob_fixups", fixups, 1);
        if (family[acc] != 1 || last_family_addr != oob) return fail("win_oob_family", family[acc], 1);
    }
    ppu_vm_size = 0;
    /* The bits of the read sync stay when the page answer changes: on an
     * ALIAS page and on a MIXED page. */
    if (vm_read_sync_mark_range(page, 0x1000u)) return fail("win_sync_mark_page", 1, 0);
    got = ppu_fm_kind[page >> PPU_FM_PAGE_SHIFT];
    if (got != (PPU_FM_K_ALIAS | PPU_FM_K_READ | PPU_FM_K_SYNC))
        return fail("win_alias_marked_bits", got, PPU_FM_K_ALIAS | PPU_FM_K_READ | PPU_FM_K_SYNC);
    sysmem_alias_record(page + 0x800u, 0x800u);       /* the page is MIXED now */
    got = ppu_fm_kind[page >> PPU_FM_PAGE_SHIFT];
    if (got != (route | PPU_FM_K_READ | PPU_FM_K_SYNC))
        return fail("win_mixed_keeps_sync", got, route | PPU_FM_K_READ | PPU_FM_K_SYNC);
    return 0;
}

#endif

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "unarmed")) return unarmed();
    reserve();
    reset();
    if (ppu_fm_slow) return fail("flag_word", ppu_fm_slow, 0);
    if (!strcmp(mode, "all")) return probes() || alias_probes();
    if (!strcmp(mode, "count")) {
        environment["PPU_FM_COUNT"] = "1";
        ppu_fm_arm();
        if (!(ppu_fm_slow & PPU_FM_S_COUNT)) return fail("count_flag", ppu_fm_slow, PPU_FM_S_COUNT);
        return probes() || counter();
    }
    if (!strcmp(mode, "diag") || !strcmp(mode, "kill")) {
        /* PPU_FM=0 is the kill switch: each access takes the family. */
        if (!strcmp(mode, "kill")) environment["PPU_FM"] = "0";
        else environment["PPU_RWATCH"] = "1";
        ppu_fm_arm();
        if (!(ppu_fm_slow & PPU_FM_S_DIAG)) return fail("diag_flag", ppu_fm_slow, PPU_FM_S_DIAG);
        return probes();
    }
    if (!strcmp(mode, "watch")) {
        environment["SPU_ATOM_EA"] = "40000";
        ppu_fm_arm();
        if (!(ppu_fm_slow & PPU_FM_S_WATCH)) return fail("watch_flag", ppu_fm_slow, PPU_FM_S_WATCH);
        return probes();
    }
    if (!strcmp(mode, "bounds")) {
        ppu_vm_size = 0x48000000u;
        ppu_fm_arm();
        return probes();
    }
    if (!strcmp(mode, "spin")) return spin_mode();
    if (!strcmp(mode, "spinoff")) {
        environment["PPU_FM_SPINDET"] = "0";
        ppu_fm_arm();
        if (ppu_fm_spin_det) return fail("spin_switch", ppu_fm_spin_det, 0);
        return spin_off();
    }
    if (!strcmp(mode, "coh")) {
        g_spu_coh_armed = 1;
        return coherent() || probes();
    }
#if PPU_FM4
    if (!strcmp(mode, "leanbits")) return lean_bits();
    if (!strcmp(mode, "leansync")) return lean_sync();
    if (!strcmp(mode, "leanalias")) return lean_alias();
#endif
    printf("FAIL mode %s unknown\n", mode);
    return 1;
}
'''

MODES = ("unarmed", "all", "count", "diag", "kill", "watch", "bounds", "spin", "spinoff", "coh")
LEAN_MODES = ("leanbits", "leansync", "leanalias")     # the lean gate (PPU_FM4)
LAYOUTS = (1, 0)     # PPU_FM_INLINE_FAST: inline at each call site, then one copy per unit
# The paths, as lists of defines: the lean gate (the default of the header),
# step 3 (PPU_FM4 0), and the path before step 3 (PPU_FM3 0).
PATHS = (("lean", ()), ("fm3", ("PPU_FM3=1", "PPU_FM4=0")), ("fm0", ("PPU_FM3=0",)))
WIDTHS = ("8", "16", "32", "64")
SYNC = Path("runtime/memory/guest_read_sync.c")
COHERENCY = Path("runtime/spu/spu_coherency.c")

# The lean gate (PPU_FM4): (name, anchor, replacement, layouts, path).
HEADER_LEAN_MUTANTS = [
    ("lean gate ignores the page byte",
     "((unsigned char)((k) | (unsigned char)ppu_fm_slow) & (unsigned char)((kind) | (mask)))",
     "((unsigned char)(ppu_fm_slow) & (unsigned char)((kind) | (mask)))", (1,), "lean"),
    ("lean gate ignores the flag word",
     "((unsigned char)((k) | (unsigned char)ppu_fm_slow) & (unsigned char)((kind) | (mask)))",
     "((unsigned char)(k) & (unsigned char)((kind) | (mask)))", (1,), "lean"),
    ("lean read gate takes the mask of a write",
     "PPU_FM_LEAN(k, PPU_FM_K_ALIAS | PPU_FM_K_READ,",
     "PPU_FM_LEAN(k, PPU_FM_K_WRITE,", (1,), "lean"),
    ("page byte: the writer sets no bit",
     "    __atomic_fetch_or(&ppu_fm_kind[page], bits, __ATOMIC_RELEASE);\n", "    (void)bits;\n",
     (1,), "lean"),
]

ARM_LEAN_MUTANTS = [
    ("lean bounds: the page before the end not flagged",
     "    if (ppu_vm_size != 0 && (uint64_t)e + PPU_FM_PAGE_SIZE + 7u > ppu_vm_size)\n",
     "    if (0)\n", (1,), "lean"),
    ("sync hook not installed", "    vm_read_sync_set_mark_hook(ppu_fm_mark_sync);\n", "", (1,), "lean"),
    ("sync hook: the page before the range not flagged",
     "    if (first > 0) ppu_fm_page_or(first - 1u, (unsigned char)PPU_FM_K_READ);\n", "", (1,), "lean"),
    ("sync hook: the last pages of the range not flagged",
     "    for (unsigned p = first; p <= last; p++)\n"
     "        ppu_fm_page_or(p, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_SYNC));\n",
     "    ppu_fm_page_or(first, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_SYNC));\n", (1,), "lean"),
    ("sync hook flags stores too",
     "        ppu_fm_page_or(p, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_SYNC));\n",
     "        ppu_fm_page_or(p, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_WRITE | PPU_FM_K_SYNC));\n",
     (1,), "lean"),
    ("sync hook: owner bit not set",
     "        ppu_fm_page_or(p, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_SYNC));\n",
     "        ppu_fm_page_or(p, (unsigned char)PPU_FM_K_READ);\n", (1,), "lean"),
]

SYNC_MUTANTS = [
    ("sync: the hook is not called", "    if(s_mark_hook)s_mark_hook(first,last);\n", "", (1,), "lean"),
    ("sync: the hook runs before the marks and the flag",
     "    for(unsigned page=first;page<=last;page++) {\n",
     (("    for(unsigned page=first;page<=last;page++) {\n",
       "    if(s_mark_hook)s_mark_hook(first,last);\n    for(unsigned page=first;page<=last;page++) {\n"),
      ("    if(s_mark_hook)s_mark_hook(first,last);\n    return 0;\n", "    return 0;\n")),
     (1,), "lean"),
]

# Step 4, move 2: the alias window answer in the page byte.
# (name, anchor, replacement, layouts, path)
WINDOW_HEADER_MUTANTS = [
    ("window: the read gate ignores the alias bit",
     "PPU_FM_LEAN(k, PPU_FM_K_ALIAS | PPU_FM_K_READ,",
     "PPU_FM_LEAN(k, PPU_FM_K_READ,", (1,), "lean"),
    ("window: the write gate ignores the alias bit",
     "PPU_FM_LEAN(k, PPU_FM_K_ALIAS | PPU_FM_K_WRITE,",
     "PPU_FM_LEAN(k, PPU_FM_K_WRITE,", (1,), "lean"),
    ("window: the alias answer ignores the page byte of the primary",
     "    if (PPU_FM_LEAN(ppu_fm_page(e >> PPU_FM_PAGE_SHIFT), kind, mask)) return 0;\n",
     "    if (0) return 0;\n", (1,), "lean"),
    ("window: the alias answer takes the masks of a store",
     "!ppu_fm_alias_ea(PPU_FM_K_READ, PPU_FM_S_READ, &ea)",
     "!ppu_fm_alias_ea(PPU_FM_K_WRITE, PPU_FM_S_WRITE, &ea)", (1,), "lean"),
]

WINDOW_ARM_MUTANTS = [
    ("window end: IDENTITY sets the alias bit", "            next |= arm;\n",
     "            next |= arm | (unsigned char)PPU_FM_K_ALIAS;\n", (1,), "lean"),
    ("window end: ALIAS flags the route bits",
     "            next |= (unsigned char)PPU_FM_K_ALIAS;\n",
     "            next |= (unsigned char)(PPU_FM_K_ALIAS | PPU_FM_K_READ | PPU_FM_K_WRITE);\n",
     (1,), "lean"),
    ("window begin sets no route bit",
     "    ppu_fm_page_or(page, (unsigned char)(PPU_FM_K_READ | PPU_FM_K_WRITE));\n",
     "    (void)page;\n", (1,), "lean"),
    ("window end: the bits of the read sync are lost",
     "        next = (old & PPU_FM_K_SYNC) ? (unsigned char)(old & sync) : 0;\n",
     "        next = 0;\n", (1,), "lean"),
    ("window end: MIXED keeps the alias answer",
     "            next |= (unsigned char)(PPU_FM_K_READ | PPU_FM_K_WRITE);\n",
     "            next |= (unsigned char)PPU_FM_K_ALIAS;\n", (1,), "lean"),
]

WINDOW_ALIAS_MUTANTS = [
    ("window: end before the page answer is stored",
     "    ppu_fm_window_begin(ea_page);\n"
     "    atomic_store_explicit(&sysmem_spu_alias_page[page], state, memory_order_release);\n"
     "    ppu_fm_window_end(ea_page, state);\n",
     "    ppu_fm_window_begin(ea_page);\n"
     "    ppu_fm_window_end(ea_page, state);\n"
     "    atomic_store_explicit(&sysmem_spu_alias_page[page], state, memory_order_release);\n",
     (1,), "lean"),
    ("window: begin not called", "    ppu_fm_window_begin(ea_page);\n", "", (1,), "lean"),
    ("window: end not called", "    ppu_fm_window_end(ea_page, state);\n", "", (1,), "lean"),
    ("window: the index in the window used as the EA page",
     "    ppu_fm_window_begin(ea_page);\n", "    ppu_fm_window_begin(page);\n", (1,), "lean"),
]

ARM_WINDOW_MUTANTS = [
    ("arm: an alias page is flagged by its own address",
     "        if (ppu_fm_kind[p] & PPU_FM_K_ALIAS) kind = 0;\n", "        if (0) kind = 0;\n", (1,), "lean"),
]

# The mutants of the paths before PPU_FM4 that must fail on the lean path too:
# their code is the same there.
ALSO_LEAN = ("read32: ref-poll fence skipped", "fire helper passes a null caller address",
             "fire dispatch ignores the caller address", "one copy per unit: null caller address",
             "byte order of write16 turned", "null test of read32 removed", "raw SPU test of read32 removed",
             "ALIAS state published as MIXED", "page table not updated", "EA bit 31 rule lost",
             "partly covered page published as IDENTITY")
# (group, mutants, key of the file text). The anchor of a mutant must occur
# once, in the header at least once.
GROUPS = (("header", HEADER_MUTANTS, "header"), ("alias", ALIAS_MUTANTS, "alias"),
          ("arm", ARM_MUTANTS, "arm"), ("header", HEADER_LEAN_MUTANTS, "header"),
          ("arm", ARM_LEAN_MUTANTS, "arm"), ("sync", SYNC_MUTANTS, "sync"),
          ("header", WINDOW_HEADER_MUTANTS, "header"), ("arm", WINDOW_ARM_MUTANTS, "arm"),
          ("alias", WINDOW_ALIAS_MUTANTS, "alias"), ("arm", ARM_WINDOW_MUTANTS, "arm"))

# The externals of the coherency file that the fixture does not use. The
# file links whole: its reserve function is a writer of the page byte. The
# coherency file of the fork declares g_spu_lane_trace and g_spu_ls_gate and
# does not define them. The copy in the game defines them in spu_context.h.
# So the stub holds a name only when no file of the tree that the coherency
# file includes defines it.
COHERENCY_STUB_NAMES = ("g_spu_lane_trace", "g_spu_ls_gate")
COHERENCY_SOURCES = (COHERENCY, Path("runtime/spu/spu_coherency.h"), Path("runtime/spu/spu_context.h"))


def coherency_stub(vendor):
    lines = []
    for path in COHERENCY_SOURCES:
        if (vendor / path).is_file():
            lines.extend((vendor / path).read_text().splitlines())
    undefined = [name for name in COHERENCY_STUB_NAMES
                 if not any(re.search(r"\b%s\s*(=[^;]*)?;" % name, line) and "extern" not in line
                            for line in lines)]
    body = "".join("uint64_t %s;\n" % name for name in undefined)
    return "#include <stdint.h>\nvoid spu_ch_wake(void);\nvoid spu_ch_wake(void) {}\n" + body


def arm_text(loader):
    begin = loader.index(ARM_BEGIN)
    end = loader.index("\n}\n", loader.index(ARM_END, begin)) + len("\n}\n")
    return loader[begin:end]


def compiler():
    found = os.environ.get("MV_CLANG") or os.environ.get("CC") or shutil.which("clang-cl") \
        or shutil.which("clang") or shutil.which("cc")
    if not found:
        raise RuntimeError("No C compiler found; set MV_CLANG or CC")
    return found


def run_command(environment, work, command):
    result = subprocess.run(command, cwd=work, env=environment, capture_output=True, text=True,
                            errors="replace", timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def build(environment, work, cc, msvc_style, texts, path_defines, vendor, inline_fast=1):
    """Writes the header copy, the arm text and the production files into
    `work`, then builds `fixture` there. texts: the keys of GROUPS.
    path_defines: the defines of the path. inline_fast: PPU_FM_INLINE_FAST."""
    work.mkdir(parents=True, exist_ok=True)
    target = work / "mut" / FASTMEM
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(texts["header"])
    (work / "fixture.cpp").write_text(FIXTURE)
    (work / "arm.inc").write_text(texts["arm"])
    (work / "alias.c").write_text(texts["alias"])
    (work / "sync.c").write_text(texts["sync"])
    (work / "coh.c").write_text(texts["coh"])
    (work / "cohstub.c").write_text(coherency_stub(vendor))
    defines = ["PPU_FM_INLINE_FAST=%d" % inline_fast] + list(path_defines)
    directories = [work / "mut" / "include", vendor / "include", vendor / "runtime" / "spu"]
    if msvc_style:
        common = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything"]
        common += ["/D" + d for d in defines] + ["/I" + p for p in map(str, directories)]
        fixture = common + ["/c", "fixture.cpp", "/Fofixture.obj"]
        plain = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything"] \
            + ["/I" + p for p in map(str, directories)]
        alias = plain + ["/Dsysmem_spu_alias_fixup=alias_fixup_impl",
                         "/Dppu_fm_window_begin=fixture_window_begin",
                         "/Dppu_fm_window_end=fixture_window_end",
                         "/c", "alias.c", "/Foalias.obj"]
        sync = plain + ["/c", "sync.c", "/Fosync.obj"]
        coh = plain + ["/c", "coh.c", "/Focoh.obj"]
        stub = plain + ["/c", "cohstub.c", "/Focohstub.obj"]
        link = [cc, "/nologo", "fixture.obj", "alias.obj", "sync.obj", "coh.obj", "cohstub.obj",
                "/Fefixture" + EXE]
    else:
        # The fixture is C++. clang, cc and gcc need their C++ drivers.
        driver = {"clang": "clang++", "cc": "c++", "gcc": "g++"}.get(Path(cc).name.lower(), cc)
        options = ["-std=c++17", "-O2", "-UNDEBUG", "-Wno-everything"]
        common = [driver] + options + ["-D" + d for d in defines] + ["-I" + p for p in map(str, directories)]
        fixture = common + ["-c", "fixture.cpp", "-o", "fixture.o"]
        plain = [cc, "-O2", "-Wno-everything"] + ["-I" + p for p in map(str, directories)]
        alias = plain + ["-Dsysmem_spu_alias_fixup=alias_fixup_impl",
                         "-Dppu_fm_window_begin=fixture_window_begin",
                         "-Dppu_fm_window_end=fixture_window_end",
                         "-c", "alias.c", "-o", "alias.o"]
        sync = plain + ["-c", "sync.c", "-o", "sync.o"]
        coh = plain + ["-c", "coh.c", "-o", "coh.o"]
        stub = plain + ["-c", "cohstub.c", "-o", "cohstub.o"]
        link = [driver, "fixture.o", "alias.o", "sync.o", "coh.o", "cohstub.o", "-o", "fixture" + EXE]
    for command in (fixture, alias, sync, coh, stub):
        run_command(environment, work, command)
    obj = "fixture.obj" if msvc_style else "fixture.o"
    copies = accessor_copies(environment, cc, work / obj)
    run_command(environment, work, link)
    return copies


# Each fixture starts through scripts/quiet_run.py (rule 1 of the grounding
# rules): a dialog of a fixture is invisible, and a fixture that blocks on a
# dialog ends at the time limit with exit code 124.
QUIET = ([sys.executable, str(ROOT / "scripts/quiet_run.py"), "--timeout", "60", "--"]
         if GAME else [])


def accessor_copies(environment, cc, obj):
    """The accessors that the compiler left out of line in the object."""
    lister = Path(cc).with_name("llvm-nm.exe") if os.name == "nt" else Path(shutil.which("nm") or "")
    assert lister.is_file(), "no symbol lister next to the compiler: %s" % lister
    result = subprocess.run([str(lister), str(obj)], capture_output=True, text=True,
                            errors="replace", env=environment, timeout=60)
    assert result.returncode == 0, result.stderr
    found = re.findall(r"ppu_fm_(read|write)(8|16|32|64)(?=@@|$)", result.stdout, re.M)
    return sorted(set(found))


def expected_copies(path, inline_fast):
    if inline_fast == 0:
        return sorted((kind, width) for kind in ("read", "write") for width in WIDTHS)
    if path == "fm0":
        return sorted(("read", width) for width in WIDTHS)
    return []


def modes_of(path):
    return MODES + (LEAN_MODES if path == "lean" else ())


def verify(work, modes, path):
    for mode in modes:
        result = subprocess.run(QUIET + [str(work / ("fixture" + EXE)), mode], capture_output=True,
                                text=True, errors="replace", timeout=240)
        assert result.returncode == 0 and not result.stdout, \
            (path, mode, result.returncode, result.stdout.strip()[-700:], result.stderr.strip()[-300:])


def reason(error):
    """The first line of a rejection: the mode and the FAIL line, or a build error."""
    text = str(error)
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in lines:
        if "FAIL" in line or "error" in line:
            return line[:150]
    return (lines[0] if lines else text)[:150]


def pairs_of(anchor, replacement):
    """The (old, new) pairs of a mutant. A tuple holds the pairs; else the
    pair is (anchor, replacement)."""
    return replacement if isinstance(replacement, tuple) else ((anchor, replacement),)


def mutant_paths(name, mutant):
    """The paths of a mutant: its own path, or the path of step 3 and, for
    ALSO_LEAN, the lean path too."""
    if len(mutant) > 4:
        return (mutant[4],)
    return ("fm3", "lean") if name in ALSO_LEAN else ("fm3",)


def apply_edits(name, text, anchor, replacement):
    for old, new in pairs_of(anchor, replacement):
        assert text.count(old) >= 1, (name, old)
        text = text.replace(old, new)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor", type=Path, default=VENDOR)
    parser.add_argument("--only", default="",
                        help="commas: the mutants with one of these texts in the name")
    parser.add_argument("--skip-actual", action="store_true",
                        help="no build of the texts without a mutant")
    args = parser.parse_args()
    key_of = dict((group, key) for group, group_mutants, key in GROUPS)
    texts = {"header": (args.vendor / FASTMEM).read_text(),
             "arm": arm_text((args.vendor / LOADER).read_text()),
             "alias": (args.vendor / ALIAS).read_text(),
             "sync": (args.vendor / SYNC).read_text(),
             "coh": (args.vendor / COHERENCY).read_text()}
    mutants = [(group, mutant) for group, group_mutants, key in GROUPS for mutant in group_mutants
               if mutant[2] is not None]
    for group, mutant in mutants:
        name = mutant[0]
        for old, new in pairs_of(mutant[1], mutant[2]):
            found = texts[key_of[group]].count(old)
            assert found >= 1 and (group == "header" or found == 1), (name, old)
    assert FIXTURE.count("#include \"ps3emu/ppu_fastmem.h\"") == 1
    assert FIXTURE.count('"arm.inc"') == 1

    if GAME:
        environment = project.build_environment()
    else:
        environment = dict(os.environ)
    cc = environment.get("MV_CLANG") or compiler()
    msvc_style = Path(cc).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")

    def make(root, path, inline_fast, parts):
        copies = build(environment, root, cc, msvc_style, parts, list(dict(PATHS)[path]), args.vendor,
                       inline_fast)
        assert copies == expected_copies(path, inline_fast), \
            "out-of-line accessors %s, want %s" % (copies, expected_copies(path, inline_fast))
        verify(root, modes_of(path), path)

    rejections = 0
    with tempfile.TemporaryDirectory(prefix="ppu-fastmem3-") as directory:
        root = Path(directory)
        if not args.skip_actual:
            for path, defines in PATHS:
                for inline_fast in LAYOUTS:
                    make(root / "actual", path, inline_fast, texts)
        for group, mutant in mutants:
            if args.only and not any(text in mutant[0] for text in args.only.split(",")):
                continue
            name, anchor, replacement = mutant[:3]
            layouts = mutant[3] if len(mutant) > 3 else (1,)
            parts = dict(texts)
            parts[key_of[group]] = apply_edits(name, texts[key_of[group]], anchor, replacement)
            for path in mutant_paths(name, mutant):
                for inline_fast in layouts:
                    try:
                        make(root / "mutant", path, inline_fast, parts)
                    except AssertionError as error:
                        rejections += 1
                        print("rejected: %s (path %s, layout %d): %s"
                              % (name, path, inline_fast, reason(error)))
                    else:
                        raise AssertionError("mutant accepted: %s (path %s, layout %d)"
                                             % (name, path, inline_fast))
    print("PASS%s: PPU fast memory path (PPU_FM3 and the PPU_FM4 lean gate), %d paths x %d layouts, "
          "%d mutants rejected" % ("" if not args.only and not args.skip_actual else " (batch)",
                                   len(PATHS), len(LAYOUTS), rejections))


if __name__ == "__main__":
    main()
