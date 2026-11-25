#!/usr/bin/env python3
"""Game-free checks for the lean fast memory paths.

1. Compiles the production ppu_fastmem.h with the production ppu_fm_arm text
   of ppu_loader.cpp and authored stubs, in both layouts (PPU_FM_INLINE_FAST
   1 and 0). Assertions: before ppu_fm_arm each access takes the
   out-of-line function; the spin detector fires once in 200000 same-address
   reads while its switch is on and never while it is off; a store into a
   reserved line takes the slow path; a store into a free line writes memory;
   an armed diagnostic sends reads and writes to the slow path; SPU_ATOM_EA
   sends stores only; each page-only condition (bounds, null pages, alias
   window, raw-SPU registers, GCM ref-poll) sends its addresses to the slow
   path and no other. Mutants of the header must fail: one for each test of
   the out-of-line function, one for each part of the gate.
2. Compiles the production bitmap test of spu_coherency.h next to the copy in
   ppu_fastmem.h. Both must agree on every tested address. Mutants of the copy
   must fail.
3. Mutants of the page table and of the flag word of ppu_fm_arm must fail.
   Only the exact value PPU_FM_SPINDET=0 turns the switch off.
4. Compiles the production PPU_FM_COUNT counter of ppu_loader.cpp with an
   authored clock. PPU_FM_COUNT arms the counter and the diagnostic family.
   The lines hold the calls of each accessor per thread and for all threads.
   A thread reads the clock one time per 16384 calls. Mutants of the header
   and of the loader must fail.
No game input; no performance claim."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

VENDOR = ROOT
FASTMEM = Path("include/ps3emu/ppu_fastmem.h")
LOADER = Path("runtime/ppu/ppu_loader.cpp")
EXE = ".exe" if os.name == "nt" else ""
ARM_BEGIN = 'extern "C" volatile uint32_t ppu_fm_slow = PPU_FM_S_ALL;'
ARM_END = "is_fm = (knobs[i][0] == 'P'"
FLAG_TEST = "if (PPU_FM_LIKELY(!ppu_fm_spin_det)) break;"
FIRE = "if (PPU_FM_UNLIKELY(++fm_->n == 200000)) { fire_args; fm_->n = 0; }"
STORE = "if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {"
BIT_TEST = "return (g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u;"
LINE_SHIFT = "#define PPU_FM_COH_LINE_SHIFT   7"
PARSE = "if (spindet && spindet[0] == '0' && !spindet[1]) ppu_fm_spin_det = 0;"

ACCESSORS = ("READ8", "READ16", "READ32", "READ64", "WRITE8", "WRITE16", "WRITE32", "WRITE64")
WIDTHS = (1, 2, 4, 8, 1, 2, 4, 8)
COUNT_HIT = ["ppu_fm_count_hit(PPU_FM_C_%s);" % name for name in ACCESSORS]
OUT_FLAG = [  # the flag test of each out-of-line function, with its count line
    "ppu_fm_slow & PPU_FM_S_%s)) {\n        if (ppu_fm_slow & PPU_FM_S_COUNT) %s" % (
        "READ" if i < 4 else "WRITE", hit)
    for i, hit in enumerate(COUNT_HIT)]

# One mutant for each test of the out-of-line functions: removed.
def out_tests():
    tests = []
    for i in range(len(ACCESSORS)):
        kind = "read" if i < 4 else "write"
        width = WIDTHS[i]
        family = "vm_" + ACCESSORS[i].lower()
        delegate = "return %s(a);" % family if kind == "read" \
            else "{ %s(a, v); return; }" % family
        tests.append(("%s%d: bounds test removed" % (kind, width),
                      "if (PPU_FM_UNLIKELY(ppu_fm_oob(ea, %d))) %s" % (width, delegate), ""))
        if kind == "read" and width != 2 or kind == "write":
            low = "0x10000u" if kind == "read" else "0x1000u"
            tests.append(("%s%d: null test removed" % (kind, width),
                          "if (PPU_FM_UNLIKELY(ea < %s))%s%s" % (
                              low, "     " if kind == "read" else " ", delegate), ""))
        if width == 4:
            tests.append(("%s32: raw SPU test removed" % kind,
                          "if (PPU_FM_UNLIKELY(spu_raw_is_reg(ea))) %s" % delegate, ""))
        if kind == "read" and width == 4:
            tests.append(("read32: ref-poll test removed",
                          "if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) ppu_fm_refpoll();", ""))
    return tests

HEADER_MUTANTS = [
    ("flag test removed", FLAG_TEST, ""),
    ("flag test ignores the switch", FLAG_TEST, "if (PPU_FM_LIKELY(ppu_fm_spin_det)) break;"),
    ("threshold changed", FIRE, FIRE.replace("200000", "300000")),
    ("equal to not equal", FIRE, FIRE.replace("== 200000", "!= 200000")),
    ("armed test removed", STORE, "if (PPU_FM_UNLIKELY(spu_coh_bitmap_test(ea))) {"),
    ("bitmap test inverted", BIT_TEST, "return !((g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u);"),
    ("line shift changed", LINE_SHIFT, LINE_SHIFT.replace("7", "6")),
    ("bit mask changed", BIT_TEST, BIT_TEST.replace("line & 7", "line & 3")),
    ("byte index changed", BIT_TEST, BIT_TEST.replace("line >> 3", "line >> 2")),
    # The gate.
    ("gate ignores the flag word",
     "((ppu_fm_slow & (mask)) | (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind)))",
     "(ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind))"),
    ("gate ignores the page table",
     "((ppu_fm_slow & (mask)) | (ppu_fm_kind[(ea) >> PPU_FM_PAGE_SHIFT] & (kind)))",
     "((ppu_fm_slow & (mask)))"),
    ("stores use the read mask",
     "if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_WRITE))) {",
     "if (PPU_FM_UNLIKELY(PPU_FM_SLOW(ea, PPU_FM_K_WRITE, PPU_FM_S_READ))) {"),
    ("reads use the write mask",
     "#define PPU_FM_S_READ  (PPU_FM_S_DIAG | PPU_FM_S_COUNT)",
     "#define PPU_FM_S_READ  PPU_FM_S_WRITE"),
    ("alias fixup removed",
     "return PPU_FM_UNLIKELY(ppu_fm_in_alias(e)) ? sysmem_spu_alias_fixup(e) : e;", "return e;"),
] + out_tests() + [
    ("flag test of the out-of-line function removed: " + name.lower(), flag, flag.replace(
        "ppu_fm_slow & PPU_FM_S_%s" % ("READ" if i < 4 else "WRITE"), "0", 1))
    for i, (name, flag) in enumerate(zip(ACCESSORS, OUT_FLAG))
] + [("count removed: " + name.lower(), hit, "") for name, hit in zip(ACCESSORS, COUNT_HIT)] + [
    ("count of %s under %s" % (a.lower(), b.lower()), ha, hb)
    for a, b, ha, hb in zip(ACCESSORS, ACCESSORS[1:], COUNT_HIT, COUNT_HIT[1:])]

BOUNDS = ("        if (ppu_vm_size != 0 && (uint64_t)e + PPU_FM_PAGE_SIZE > ppu_vm_size)\n")
WATCH = "    if (atom && ((uint32_t)strtoul(atom, 0, 16) & ~127u)) slow |= PPU_FM_S_WATCH;\n"
LOADER_MUTANTS = [
    ("parse accepts 00", PARSE, "if (spindet && spindet[0] == '0') ppu_fm_spin_det = 0;"),
    ("parse removed", PARSE, ""),
    ("parse accepts a leading zero word", PARSE, PARSE.replace("!spindet[1]", "spindet[1] != 'x'")),
    ("default off", 'extern "C" volatile uint32_t ppu_fm_spin_det = 1;',
     'extern "C" volatile uint32_t ppu_fm_spin_det = 0;'),
    # The page table.
    ("pages outside ppu_vm_size not flagged", BOUNDS, "        if (0)\n"),
    ("page that starts inside ppu_vm_size not flagged", BOUNDS,
     "        if (ppu_vm_size != 0 && (uint64_t)e > ppu_vm_size)\n"),
    ("null-read pages below 0x1000 only",
     "        if (e < 0x10000u) kind |= (unsigned char)PPU_FM_K_READ;",
     "        if (e < 0x1000u) kind |= (unsigned char)PPU_FM_K_READ;"),
    ("null-store page not flagged",
     "        if (e < 0x1000u)  kind |= (unsigned char)PPU_FM_K_WRITE;", ""),
    ("alias pages not flagged", "        if (ppu_fm_in_alias(e))\n", "        if (0)\n"),
    ("raw SPU register pages not flagged", "        if (spu_raw_is_reg(e))\n", "        if (0)\n"),
    ("ref-poll page not flagged",
     "        if (e == refpoll) kind |= (unsigned char)PPU_FM_K_READ;", ""),
    # The flag word.
    ("SPU_ATOM_EA does not send stores out", WATCH, ""),
    ("SPU_ATOM_EA=0 sends stores out", WATCH, "    if (atom) slow |= PPU_FM_S_WATCH;\n"),
    ("PPU_FM=0 keeps the fast paths",
     "if (is_fm) { if (e[0] == '0') { slow |= PPU_FM_S_DIAG; break; } continue; }",
     "if (is_fm) { continue; }"),
    ("a knob does not send accesses out", "        slow |= PPU_FM_S_DIAG; break;\n", "        break;\n"),
    ("flag word not stored", "    ppu_fm_slow = slow;\n", "    ppu_fm_slow = 0;\n"),
    ("flag word starts clear", ARM_BEGIN,
     'extern "C" volatile uint32_t ppu_fm_slow = 0;'),
    # The counter.
    ("PPU_FM_COUNT=0 turns the counter on", "if (count && (count[0] != '0' || count[1])) {", "if (count) {"),
    ("PPU_FM_COUNT does not arm the counter", "        slow |= PPU_FM_S_COUNT;\n", ""),
    ("line too early", "    if (now - last < frequency) return;", "    if (now - last < frequency / 2) return;"),
    ("period changed", "#define PPU_FM_COUNT_PERIOD 16384", "#define PPU_FM_COUNT_PERIOD 32768"),
    ("slot of a thread not kept", "        ppu_fm_count_slot = s;\n", ""),
    ("thread line without delta", "            line[k] = now - ppu_fm_count_prev[s][k];", "            line[k] = now;"),
    ("all line without sum", "            all[k] += line[k];\n", ""),
    ("thread line without calls", "        if (!any) continue;", ""),
    ("sequence number not increased", "    const uint32_t seq = ppu_fm_count_seq++;",
     "    const uint32_t seq = ppu_fm_count_seq;"),
    ("first second does not start", "    if (last == 0) {", "    if (1) {"),
]

READ_SYNC = r'''
#ifndef VM_READ_SYNC_STUB_H
#define VM_READ_SYNC_STUB_H
#include <stdint.h>
static inline void vm_cpu_read_sync(uint64_t address, unsigned bytes)
{ (void)address; (void)bytes; }
#endif
'''

# The runtime state and the slow paths that ppu_fastmem.h calls. The flag
# word, the page table and the detector switch come from the arm text.
STUBS = r'''
/* ---- authored stubs of the runtime state ---- */
#define MEM_BYTES 0x4000000u       /* guest window of the fixture: 64 MB */
#define EA        0x123456u        /* an index into memory when vm_base is memory */
static unsigned char* memory;      /* allocated in reset(); vm_base moves below it */
uint8_t*  vm_base;
uint32_t  ppu_vm_size = 0;
uint32_t  ppu_hle_inject_base = 0x7FE00000u;
volatile int g_spu_coh_armed = 0;
unsigned char g_spu_coh_bitmap[PPU_FM_COH_BITMAP_BYTES];
static int slow_reads, slow_writes, fires, refpolls, fixups, alias_on;
static std::map<std::string, std::string> environment;
/* ---- authored clock, thread ids and output of the PPU_FM_COUNT counter ---- */
static uint64_t fake_now = 5, fake_guest = 0x1001u;
static unsigned long fake_tid = 11;
static std::string fake_lines;
uint64_t ppu_fm_count_now(void) { return fake_now; }
uint64_t ppu_fm_count_frequency(void) { return 1000000u; }
unsigned long ppu_fm_count_host_tid(void) { return fake_tid; }
uint64_t ppu_fm_count_guest_tid(void) { return fake_guest; }
void ppu_fm_count_emit(const char* line) { fake_lines += line; }

extern "C" {
/* 0x46xxxxxx, an address of the alias window, maps to 0x44xxxxxx, below the window. */
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { fixups++; return alias_on ? ((ea & 0xF0FFFFFFu) | 0x04000000u) : ea; }
uint8_t  vm_read8 (uint64_t) { slow_reads++; return 0x5A; }
uint16_t vm_read16(uint64_t) { slow_reads++; return 0x5A5A; }
uint32_t vm_read32(uint64_t) { slow_reads++; return 0x5A5A5A5Au; }
uint64_t vm_read64(uint64_t) { slow_reads++; return 0x5A5A5A5A5A5A5A5Aull; }
void vm_write8 (uint64_t, uint8_t)  { slow_writes++; }
void vm_write16(uint64_t, uint16_t) { slow_writes++; }
void vm_write32(uint64_t, uint32_t) { slow_writes++; }
void vm_write64(uint64_t, uint64_t) { slow_writes++; }
void ppu_fm_refpoll(void) { refpolls++; }
void ppu_fm_hotread8 (uint32_t, void*) { fires++; }
void ppu_fm_hotread16(uint32_t, void*) { fires++; }
void ppu_fm_hotread32(uint32_t, uint32_t, void*) { fires++; }
void ppu_fm_hotread64(uint32_t, uint64_t, void*) { fires++; }
}
'''
FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include "ps3emu/ppu_fastmem.h"
#include "arm.inc"

''' + STUBS + r'''
char* getenv(const char* name)
{
    std::map<std::string, std::string>::const_iterator it = environment.find(name);
    return it == environment.end() ? 0 : const_cast<char*>(it->second.c_str());
}

/* The bitmap test as ppu_fastmem.h defines it. */
extern "C" int fast_test(uint32_t addr) { return spu_coh_bitmap_test(addr); }

static void set_line(int on)
{
    uint32_t line = EA >> 7;
    if (on) g_spu_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    else    g_spu_coh_bitmap[line >> 3] &= (unsigned char)~(1u << (line & 7));
}

/* Puts guest address `base` at the start of memory. Each mode names the
 * addresses it reads; the window is 64 MB, so no access leaves it. */
static void set_base(uint32_t base) { vm_base = memory - base; }

/* No knob in the environment, the bounds off, then the production arm. */
static void reset(void)
{
    slow_reads = slow_writes = fires = refpolls = fixups = 0;
    alias_on = 0; ppu_vm_size = 0; g_spu_coh_armed = 0; ppu_fm_spin_det = 1;
    memset(g_spu_coh_bitmap, 0, PPU_FM_COH_BITMAP_BYTES);
    if (!memory) memory = (unsigned char*)calloc(MEM_BYTES, 1);
    set_base(0);
    memset(memory, 0, MEM_BYTES);
    environment.clear();
    ppu_fm_arm();
}

static void reads(int count)
{
    for (int i = 0; i < count; i++) (void)ppu_fm_read32(EA);
}

static int fail(const char* what, int value, int expected)
{
    printf("%s=%d expected=%d\n", what, value, expected);
    return 1;
}

static int fail_lines(const char* what)
{
    printf("%s: got\n%s", what, fake_lines.c_str());
    return 1;
}

/* The columns of a line, authored here. */
static std::string columns(int r8, int r16, int r32, int r64, int w8, int w16, int w32, int w64)
{
    char text[256];
    snprintf(text, sizeof text,
             "read8=%d read16=%d read32=%d read64=%d write8=%d write16=%d write32=%d write64=%d calls=%d\n",
             r8, r16, r32, r64, w8, w16, w32, w64, r8 + r16 + r32 + r64 + w8 + w16 + w32 + w64);
    return text;
}

static std::string thread_line(int seq, int slot, unsigned long tid, uint64_t guest, const char* tail)
{
    char head[160];
    snprintf(head, sizeof head, "[PPU_FM_COUNT] seq=%d scope=thread slot=%d tid=%lu guest=%llu ",
             seq, slot, tid, (unsigned long long)guest);
    return std::string(head) + tail;
}

static std::string all_line(int seq, uint64_t start, uint64_t end, const char* tail)
{
    char head[200];
    snprintf(head, sizeof head, "[PPU_FM_COUNT] seq=%d scope=all start=%llu end=%llu frequency=%llu ",
             seq, (unsigned long long)start, (unsigned long long)end, 1000000ull);
    return std::string(head) + tail;
}

static void count_calls(int r8, int r16, int r32, int r64, int w8, int w16, int w32, int w64)
{
    for (int i = 0; i < r8; i++)  (void)ppu_fm_read8(EA);
    for (int i = 0; i < r16; i++) (void)ppu_fm_read16(EA);
    for (int i = 0; i < r32; i++) (void)ppu_fm_read32(EA);
    for (int i = 0; i < r64; i++) (void)ppu_fm_read64(EA);
    for (int i = 0; i < w8; i++)  ppu_fm_write8(EA, 1);
    for (int i = 0; i < w16; i++) ppu_fm_write16(EA, 1);
    for (int i = 0; i < w32; i++) ppu_fm_write32(EA, 1);
    for (int i = 0; i < w64; i++) ppu_fm_write64(EA, 1);
}

/* The period of the clock read, authored here. */
static const int PERIOD = 16384;

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    const uint32_t initial_spin_det = ppu_fm_spin_det;   /* the definition, before any test */
    if (strcmp(mode, "unarmed")) reset();
    if (!strcmp(mode, "unarmed")) {
        /* Before ppu_fm_arm: each bit of the flag word is set. */
        if (ppu_fm_slow != PPU_FM_S_ALL) return fail("initial_flag_word", (int)ppu_fm_slow, PPU_FM_S_ALL);
        memory = (unsigned char*)calloc(MEM_BYTES, 1);
        set_base(0);
        (void)ppu_fm_read32(EA); ppu_fm_write32(EA, 1);
        if (slow_reads != 1 || slow_writes != 1) return fail("before_arm", slow_reads, 1);
        environment.clear();
        ppu_fm_arm();
        (void)ppu_fm_read32(EA); ppu_fm_write32(EA, 1);
        if (slow_reads != 1 || slow_writes != 1) return fail("after_arm", slow_reads, 1);
        if (fires) return fail("fires", fires, 0);
    } else if (!strcmp(mode, "spin_on")) {
        /* The first read of an address only records it. Reads after it count. */
        (void)ppu_fm_read32(EA);
        reads(199999);
        if (fires != 0) return fail("fires_after_199999", fires, 0);
        reads(1);
        if (fires != 1) return fail("fires_after_200000", fires, 1);
        reads(199999);
        if (fires != 1) return fail("fires_before_400000", fires, 1);
        reads(1);
        if (fires != 2) return fail("fires_after_400000", fires, 2);
        (void)ppu_fm_read8(EA); (void)ppu_fm_read16(EA); (void)ppu_fm_read64(EA);
        (void)ppu_fm_read32(EA + 4);
        if (fires != 2) return fail("fires_other_addresses", fires, 2);
        if (slow_reads) return fail("slow_reads", slow_reads, 0);
    } else if (!strcmp(mode, "spin_off")) {
        /* The production switch, by the environment. */
        environment["PPU_FM_SPINDET"] = "0";
        ppu_fm_arm();
        reads(300000);
        (void)ppu_fm_read8(EA); (void)ppu_fm_read16(EA); (void)ppu_fm_read64(EA);
        if (fires) return fail("fires", fires, 0);
        if (slow_reads) return fail("slow_reads", slow_reads, 0);
    } else if (!strcmp(mode, "store")) {
        for (int width = 0; width < 4; width++) {
            for (int on = 0; on < 2; on++) {
                reset();
                g_spu_coh_armed = 1; set_line(on);
                if (width == 0) ppu_fm_write8 (EA, 0x11);
                if (width == 1) ppu_fm_write16(EA, 0x1122);
                if (width == 2) ppu_fm_write32(EA, 0x11223344u);
                if (width == 3) ppu_fm_write64(EA, 0x1122334455667788ull);
                if (slow_writes != on) return fail("slow_writes", slow_writes, on);
                /* Guest memory is big-endian: the store writes the bytes in order. */
                if (on) { if (memory[EA]) return fail("reserved_line_written", memory[EA], 0); }
                else {
                    int bytes = 1 << width;
                    static const unsigned char expected[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
                    for (int i = 0; i < bytes; i++)
                        if (memory[EA + i] != expected[i]) return fail("free_line_byte", memory[EA + i], expected[i]);
                }
            }
            /* Armed zero: the bitmap bit is not read, the store writes memory. */
            reset(); g_spu_coh_armed = 0; set_line(1);
            if (width == 0) ppu_fm_write8 (EA, 0x11);
            if (width == 1) ppu_fm_write16(EA, 0x1122);
            if (width == 2) ppu_fm_write32(EA, 0x11223344u);
            if (width == 3) ppu_fm_write64(EA, 0x1122334455667788ull);
            if (slow_writes) return fail("slow_writes_unarmed", slow_writes, 0);
            if (!memory[EA]) return fail("unarmed_line_not_written", memory[EA], 1);
        }
    } else if (!strcmp(mode, "diag")) {
        /* A knob in the environment, by the production arm: each access goes
         * to the family, and no page test comes before that. */
        environment["PPU_RWATCH"] = "1";
        ppu_fm_arm();
        (void)ppu_fm_read32(0x2000u);
        if (slow_reads != 1) return fail("diag_null_page_read", slow_reads, 1);
        (void)ppu_fm_read32(EA); (void)ppu_fm_read8(EA); (void)ppu_fm_read16(EA); (void)ppu_fm_read64(EA);
        if (slow_reads != 5) return fail("diag_reads", slow_reads, 5);
        if (fires) return fail("diag_fires", fires, 0);
        ppu_fm_write8(EA, 1); ppu_fm_write16(EA, 1); ppu_fm_write32(EA, 1); ppu_fm_write64(EA, 1);
        if (slow_writes != 4) return fail("diag_writes", slow_writes, 4);
        if (memory[EA]) return fail("diag_wrote_memory", memory[EA], 0);
        reset();
        environment["PPU_FM"] = "0";             /* the kill switch */
        ppu_fm_arm();
        (void)ppu_fm_read32(EA); ppu_fm_write32(EA, 1);
        if (slow_reads != 1 || slow_writes != 1) return fail("kill_switch", slow_reads, 1);
        if (refpolls) return fail("refpolls", refpolls, 0);
    } else if (!strcmp(mode, "watch")) {
        /* SPU_ATOM_EA sends stores to the family. Reads keep the fast path. */
        environment["SPU_ATOM_EA"] = "40000";
        ppu_fm_arm();
        ppu_fm_write8(EA, 1); ppu_fm_write16(EA, 1); ppu_fm_write32(EA, 1); ppu_fm_write64(EA, 1);
        if (slow_writes != 4) return fail("watch_writes", slow_writes, 4);
        if (memory[EA]) return fail("watch_wrote_memory", memory[EA], 0);
        (void)ppu_fm_read32(EA); (void)ppu_fm_read8(EA); (void)ppu_fm_read16(EA); (void)ppu_fm_read64(EA);
        if (slow_reads) return fail("watch_reads", slow_reads, 0);
        reset();
        environment["SPU_ATOM_EA"] = "0";        /* no address: no watch */
        ppu_fm_arm();
        ppu_fm_write32(EA, 1);
        if (slow_writes || memory[EA + 3] != 1) return fail("watch_zero", slow_writes, 0);
    } else if (!strcmp(mode, "refpoll")) {
        set_base(ppu_hle_inject_base);
        ppu_fm_arm();
        (void)ppu_fm_read32(ppu_hle_inject_base + 0x2008u);
        if (refpolls != 1) return fail("refpolls", refpolls, 1);
        /* The page is flagged; the other addresses of it take the fast body. */
        (void)ppu_fm_read32(ppu_hle_inject_base + 0x200Cu);
        (void)ppu_fm_read64(ppu_hle_inject_base + 0x2000u);
        (void)ppu_fm_read8(ppu_hle_inject_base + 0x2FFFu);
        if (refpolls != 1 || slow_reads) return fail("refpolls_other", refpolls * 1000 + slow_reads, 1000);
    } else if (!strcmp(mode, "null")) {
        (void)ppu_fm_read32(0x2000u); (void)ppu_fm_read8(0xF000u); (void)ppu_fm_read64(0xFFF8u);
        if (slow_reads != 3) return fail("null_reads", slow_reads, 3);
        (void)ppu_fm_read16(0x2002u);            /* read16 has no null-page test */
        if (slow_reads != 3) return fail("null_read16", slow_reads, 3);
        ppu_fm_write8(0x800u, 1); ppu_fm_write16(0x802u, 1); ppu_fm_write32(0x804u, 1); ppu_fm_write64(0x808u, 1);
        if (slow_writes != 4) return fail("null_writes", slow_writes, 4);
        if (memory[0x800]) return fail("null_write_wrote_memory", memory[0x800], 0);
        ppu_fm_write32(0x1000u, 1);              /* above the null-store page */
        if (slow_writes != 4 || memory[0x1003] != 1) return fail("store_at_0x1000", slow_writes, 4);
        (void)ppu_fm_read32(0x10000u);           /* above the null-read pages */
        if (slow_reads != 3) return fail("read_at_0x10000", slow_reads, 3);
        if (fires) return fail("fires", fires, 0);
    } else if (!strcmp(mode, "alias")) {
        /* An address of the alias window goes out, gets the fixup, then the
         * fast body at the fixed address. */
        static const unsigned char bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        set_base(0x44000000u); alias_on = 1; ppu_fm_arm();
        memcpy(memory + EA, bytes, 8);
        memory[0] = 0x7B;
        if (ppu_fm_read32(0x46123456u) != 0x11223344u) return fail("alias_read32", 0, 0x11223344);
        if (ppu_fm_read64(0x46123456u) != 0x1122334455667788ull) return fail("alias_read64", 0, 1);
        ppu_fm_write32(0x4612345Au, 0xAABBCCDDu);
        if (memory[EA + 4] != 0xAA || memory[EA + 7] != 0xDD) return fail("alias_write32", memory[EA + 4], 0xAA);
        if (fixups != 3 || slow_reads || slow_writes) return fail("alias_routes", fixups * 1000 + slow_reads * 10 + slow_writes, 3000);
        (void)ppu_fm_read8(0x46F00000u);        /* another page of the window */
        if (fixups != 4) return fail("alias_other_page", fixups, 4);
    } else if (!strcmp(mode, "raw")) {
        /* Raw SPU problem state: read32 and write32 go to the family. */
        set_base(0xE0000000u); ppu_fm_arm();
        (void)ppu_fm_read32(0xE0044004u);        /* outbound mailbox of slot 0: a read pops it */
        ppu_fm_write32(0xE004401Cu, 1);          /* run control */
        if (slow_reads != 1 || slow_writes != 1) return fail("raw_register", slow_reads, 1);
        (void)ppu_fm_read32(0xE0140000u);        /* slot 1 */
        if (slow_reads != 2) return fail("raw_slot1", slow_reads, 2);
        (void)ppu_fm_read32(0xE0030000u);        /* local store: plain memory */
        (void)ppu_fm_read8(0xE0044004u);         /* the exact test is read32 and write32 only */
        (void)ppu_fm_read16(0xE0044004u); (void)ppu_fm_read64(0xE0044004u);
        ppu_fm_write8(0xE004401Cu, 1); ppu_fm_write16(0xE004401Cu, 1);
        ppu_fm_write64(0xE004401Cu, 1);
        (void)ppu_fm_read32(0xE0600000u);        /* one slot past the last: no register */
        if (slow_reads != 2 || slow_writes != 1) return fail("raw_plain", slow_reads * 10 + slow_writes, 21);
    } else if (!strcmp(mode, "bounds")) {
        /* ppu_vm_size is not a page boundary. Each page that is not entirely
         * inside it is flagged; the out-of-line function makes the exact test. */
        ppu_vm_size = 0x123456u;
        ppu_fm_arm();
        (void)ppu_fm_read8(0x123455u); (void)ppu_fm_read16(0x123454u);
        (void)ppu_fm_read32(0x123452u); (void)ppu_fm_read64(0x12344Eu);
        ppu_fm_write8(0x123455u, 1); ppu_fm_write16(0x123454u, 1);
        ppu_fm_write32(0x123452u, 1); ppu_fm_write64(0x12344Eu, 1);
        if (slow_reads || slow_writes || memory[0x123455] != 1) return fail("inside", slow_reads * 10 + slow_writes, 0);
        (void)ppu_fm_read8(0x123456u); (void)ppu_fm_read16(0x123455u);
        (void)ppu_fm_read32(0x123454u); (void)ppu_fm_read64(0x123450u);
        ppu_fm_write8(0x123456u, 1); ppu_fm_write16(0x123455u, 1);
        ppu_fm_write32(0x123454u, 1); ppu_fm_write64(0x123450u, 1);
        if (slow_reads != 4 || slow_writes != 4) return fail("outside", slow_reads * 10 + slow_writes, 44);
        if (memory[0x123456]) return fail("outside_wrote_memory", memory[0x123456], 0);
        (void)ppu_fm_read32(0x122FFCu);          /* a page entirely inside */
        if (slow_reads != 4) return fail("page_inside", slow_reads, 4);
        ppu_vm_size = 0;                         /* a second arm after a change */
        ppu_fm_arm();
        (void)ppu_fm_read32(0x123454u);
        if (slow_reads != 4) return fail("after_second_arm", slow_reads, 4);
    } else if (!strcmp(mode, "arm")) {
        const char* unset = 0;
        if (initial_spin_det != 1) return fail("default", (int)initial_spin_det, 1);
        const char* values[] = {"0", "1", "00", "0 ", " 0", "false", "", "10", "0x0", "0.0", unset};
        for (int i = 0; i < 10; i++) {
            ppu_fm_spin_det = 1;
            environment.clear();
            if (values[i]) environment["PPU_FM_SPINDET"] = values[i];
            ppu_fm_arm();
            if (ppu_fm_spin_det != (i == 0 ? 0u : 1u)) return fail("switch", i, ppu_fm_spin_det);
            if (ppu_fm_slow) return fail("flag_word", ppu_fm_slow, 0);
        }
        /* The flag word, one bit per knob. */
        static const struct { const char* name; const char* value; uint32_t flags; } cases[] = {
            {"PPU_FM", "0", PPU_FM_S_DIAG}, {"PPU_FM", "1", 0}, {"PPU_FM", "00", PPU_FM_S_DIAG},
            {"PPU_FM", "false", 0},
            {"PPU_RWATCH", "1", PPU_FM_S_DIAG}, {"PPU_HOTMAP", "", PPU_FM_S_DIAG},
            {"SPU_WP", "0", 0}, {"SPU_WP", "1", PPU_FM_S_DIAG},
            {"SPU_ATOM_EA", "40000", PPU_FM_S_WATCH}, {"SPU_ATOM_EA", "0", 0}, {"SPU_ATOM_EA", "40", 0},
            {"PS3_NULL_SWEEP", "1", PPU_FM_S_DIAG},
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            environment.clear();
            environment[cases[i].name] = cases[i].value;
            ppu_fm_arm();
            if (ppu_fm_slow != cases[i].flags) return fail("knob", i, cases[i].flags);
        }
        /* PPU_FM=0 is the kill switch and does not move the spin switch. */
        environment.clear(); environment["PPU_FM"] = "0";
        ppu_fm_arm();
        if (ppu_fm_slow != PPU_FM_S_DIAG || ppu_fm_spin_det != 1) return fail("kill_switch", (int)ppu_fm_slow, PPU_FM_S_DIAG);
        /* Two knobs: two bits. */
        environment.clear(); environment["PPU_RWATCH"] = "1"; environment["SPU_ATOM_EA"] = "40000";
        ppu_fm_arm();
        if (ppu_fm_slow != (PPU_FM_S_DIAG | PPU_FM_S_WATCH)) return fail("two_knobs", (int)ppu_fm_slow, PPU_FM_S_DIAG | PPU_FM_S_WATCH);
    } else if (!strcmp(mode, "count")) {
        /* With the switch off nothing is counted, also not on the diagnostic path. */
        environment["PPU_RWATCH"] = "1";
        ppu_fm_arm();
        (void)ppu_fm_read32(EA);
        if (ppu_fm_count_slots[0].used) return fail("counted_when_off", 1, 0);
        /* Absent or 0 is off. Any other value is on, and arms the counter. */
        const char* values[] = {"0", "", "1", 0};
        static const int armed[] = {0, 1, 1};
        for (int i = 0; i < 3; i++) {
            fake_lines.clear();
            environment.clear();
            environment["PPU_FM_COUNT"] = values[i];
            ppu_fm_arm();
            if ((ppu_fm_slow & PPU_FM_S_COUNT) != (uint32_t)armed[i] * PPU_FM_S_COUNT)
                return fail("switch", i, armed[i]);
            if (armed[i] ? fake_lines != "[PPU_FM_COUNT] on\n" : !fake_lines.empty())
                return fail_lines("on_line");
        }
        environment.clear();
        ppu_fm_arm();
        if (ppu_fm_slow & PPU_FM_S_COUNT) return fail("switch_absent", (int)ppu_fm_slow, 0);
        /* Armed. Every accessor gives its call to the family and is counted in its column. */
        environment["PPU_FM_COUNT"] = "1";
        ppu_fm_count_slot = -1; fake_lines.clear();
        fake_now = 5; fake_tid = 11; fake_guest = 0x1001u;
        ppu_fm_arm();
        fake_lines.clear();              /* the on line is checked above */
        slow_reads = slow_writes = 0;
        count_calls(1, 2, 3, 4, 5, 6, 7, 8);
        if (slow_reads != 10 || slow_writes != 26) return fail("count_delegates", slow_reads, 10);
        ppu_fm_count_tick();             /* the first clock read starts the first second */
        if (!fake_lines.empty()) return fail_lines("line_at_start");
        fake_now += 1000000u;            /* one second */
        ppu_fm_count_tick();
        std::string expected = thread_line(0, 0, 11, 0x1001u, columns(1, 2, 3, 4, 5, 6, 7, 8).c_str()) +
                               all_line(0, 5, 1000005u, columns(1, 2, 3, 4, 5, 6, 7, 8).c_str());
        if (fake_lines != expected) return fail_lines("first_second");
        /* A second thread takes slot 1. The slot is thread-local, so the test
         * starts the thread by hand. The first thread has no calls in this
         * second, so it has no line. */
        fake_lines.clear();
        ppu_fm_count_slot = -1; fake_tid = 12; fake_guest = 0;
        count_calls(0, 0, 0, 1, 1, 0, 0, 0);
        fake_now += 1000000u;
        ppu_fm_count_tick();
        expected = thread_line(1, 1, 12, 0, columns(0, 0, 0, 1, 1, 0, 0, 0).c_str()) +
                   all_line(1, 1000005u, 2000005u, columns(0, 0, 0, 1, 1, 0, 0, 0).c_str());
        if (fake_lines != expected) return fail_lines("second_second");
        /* Less than one second since the last line: no line. */
        fake_lines.clear();
        fake_now += 999999u;
        ppu_fm_count_tick();
        if (!fake_lines.empty()) return fail_lines("line_too_early");
        /* The periodic path reads the clock at call PPU_FM_COUNT_PERIOD of a
         * thread. This thread has 2 calls. */
        fake_now += 1;                   /* one second since the last line */
        count_calls(0, 0, PERIOD - 3, 0, 0, 0, 0, 0);
        if (!fake_lines.empty()) return fail_lines("line_before_period");
        (void)ppu_fm_read32(EA);
        expected = thread_line(2, 1, 12, 0, columns(0, 0, PERIOD - 2, 0, 0, 0, 0, 0).c_str()) +
                   all_line(2, 2000005u, 3000005u, columns(0, 0, PERIOD - 2, 0, 0, 0, 0, 0).c_str());
        if (fake_lines != expected) return fail_lines("periodic_second");
    } else if (!strcmp(mode, "agree")) {
        return 0;
    } else {
        return fail("mode", 0, 0);
    }
    if (refpolls && strcmp(mode, "refpoll")) return fail("refpolls", refpolls, 0);
    return 0;
}
'''
# The bitmap test as spu_coherency.h defines it.
COHERENT = r'''
#include <stdint.h>
#include "spu_coherency.h"
extern "C" int prod_test(uint32_t addr) { return spu_coh_bitmap_test(addr); }
'''
# Compares the two tests. The addresses cover all bit positions of a byte,
# the ends of the address space, and a fixed sequence of other addresses.
AGREE = r'''
#include <stdio.h>
#include <stdint.h>
#include <map>
#include <string>
#include "ps3emu/ppu_fastmem.h"
''' + STUBS + r'''
/* The bitmap test as ppu_fastmem.h defines it. */
extern "C" int fast_test(uint32_t addr) { return spu_coh_bitmap_test(addr); }
extern "C" int prod_test(uint32_t addr);
int main(void)
{
    uint32_t seed = 0x9E3779B9u, address = 0, checked = 0;
    for (uint32_t i = 0; i < (1u << 25); i += 7919u) g_spu_coh_bitmap[i >> 3] ^= (unsigned char)(1u << (i & 7));
    for (uint32_t i = 0; i < 3000000u; i++) {
        int a = fast_test(address), b = prod_test(address);
        if (a != b) { printf("address=%08x fast=%d prod=%d\n", address, a, b); return 1; }
        checked++;
        address = i < 4096u ? i * 128u : address * 1664525u + 1013904223u + seed;
        seed ^= seed << 13;
    }
    printf("checked=%u\n", checked);
    return checked == 3000000u ? 0 : 1;
}
'''

MODES = ("unarmed", "spin_on", "spin_off", "store", "diag", "watch", "refpoll", "null", "alias",
         "raw", "bounds", "arm", "count")
LAYOUTS = (1, 0)     # PPU_FM_INLINE_FAST: inline at each call site, then one copy per unit


def arm_text(loader):
    begin = loader.index(ARM_BEGIN)
    end = loader.index("\n}\n", loader.index(ARM_END, begin)) + len("\n}\n")
    return loader[begin:end]


def compile(environment, work, command):
    work.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command, cwd=work, env=environment, capture_output=True, text=True,
                            errors="replace", timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr


def compiler():
    found = os.environ.get("MV_CLANG") or os.environ.get("CC") or shutil.which("clang-cl") \
        or shutil.which("clang") or shutil.which("cc")
    if not found:
        raise RuntimeError("No C compiler found; set MV_CLANG or CC")
    return found


def build(environment, work, cc, msvc_style, header, arm, vendor, inline_fast):
    """Writes the header, the stub header and the sources into `work`, then
    builds `fixture` and `agree` there."""
    work.mkdir(parents=True, exist_ok=True)
    for path, text in ((FASTMEM, header), (Path("include/ps3emu/guest_read_sync.h"), READ_SYNC)):
        target = work / "mut" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    for name, text in (("fixture.cpp", FIXTURE), ("coherent.cpp", COHERENT), ("agree.cpp", AGREE)):
        (work / name).write_text(text)
    (work / "arm.inc").write_text(arm)
    directories = [work / "mut" / "include", vendor / "include", vendor / "runtime" / "spu"]
    # This check is the contract of the layout before step 3 (PPU_FM3 0).
    # tests/check_ppu_fastmem3.py holds the contract of PPU_FM3 1.
    defines = ["PPU_FM_INLINE_FAST=%d" % inline_fast, "PPU_FM3=0"]
    if msvc_style:
        common = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything"] + ["/D" + d for d in defines]
        common += ["/I" + str(path) for path in directories] + ["/Fo" + str(work) + os.sep]
        fixture = [str(work / "fixture.cpp"), "/Fe" + str(work / ("fixture" + EXE))]
        agree = [str(work / "coherent.cpp"), str(work / "agree.cpp"),
                 "/Fe" + str(work / ("agree" + EXE))]
    else:
        # The sources are C++. clang, cc and gcc need their C++ drivers.
        driver = {"clang": "clang++", "cc": "c++", "gcc": "g++"}.get(Path(cc).name.lower(), cc)
        common = [driver, "-std=c++17", "-O2", "-UNDEBUG", "-Wno-everything"] + ["-D" + d for d in defines]
        common += ["-I" + str(path) for path in directories]
        fixture = [str(work / "fixture.cpp"), "-o", str(work / ("fixture" + EXE))]
        agree = [str(work / "coherent.cpp"), str(work / "agree.cpp"),
                 "-o", str(work / ("agree" + EXE))]
    compile(environment, work, common + fixture)
    compile(environment, work, common + agree)
    return work


def run(exe):
    result = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace", timeout=120)
    assert result.returncode == 0 and result.stdout.strip() == "checked=3000000", \
        (result.returncode, result.stdout, result.stderr)


def verify(work):
    for mode in MODES:
        result = subprocess.run([str(work / ("fixture" + EXE)), mode], capture_output=True, text=True,
                                errors="replace", timeout=120)
        assert result.returncode == 0 and not result.stdout, (mode, result.returncode, result.stdout, result.stderr)
    run(work / ("agree" + EXE))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor", type=Path, default=VENDOR)
    args = parser.parse_args()
    header = (args.vendor / FASTMEM).read_text()
    loader = (args.vendor / LOADER).read_text()
    arm = arm_text(loader)
    for name, anchor, _ in HEADER_MUTANTS:
        assert header.count(anchor) >= 1, (name, anchor)
    for name, anchor, _ in LOADER_MUTANTS:
        assert arm.count(anchor) == 1, (name, anchor)

    def build_all(root, header_text, arm_text_):
        for inline_fast in LAYOUTS:
            build(dict(os.environ), root / ("layout%d" % inline_fast), cc, msvc_style, header_text,
                  arm_text_, args.vendor, inline_fast)
        return root

    def verify_tree(root):
        for inline_fast in LAYOUTS:
            verify(root / ("layout%d" % inline_fast))

    cc = compiler()
    msvc_style = Path(cc).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    with tempfile.TemporaryDirectory(prefix="ppu-fastmem-diet-") as directory:
        root = Path(directory)
        verify_tree(build_all(root / "actual", header, arm))
        rejected = 0
        for name, anchor, replacement in HEADER_MUTANTS:
            try:
                verify_tree(build_all(root / "header", header.replace(anchor, replacement), arm))
            except AssertionError:
                rejected += 1
            else:
                raise AssertionError("header mutant accepted: " + name)
        for name, anchor, replacement in LOADER_MUTANTS:
            try:
                verify(build(dict(os.environ), root / "loader" / "layout1", cc, msvc_style, header,
                                 arm.replace(anchor, replacement), args.vendor, 1))
            except AssertionError:
                rejected += 1
            else:
                raise AssertionError("loader mutant accepted: " + name)
    print("PASS: fast memory paths, %d header mutants and %d loader mutants rejected"
          % (len(HEADER_MUTANTS), len(LOADER_MUTANTS)))


if __name__ == "__main__":
    main()
