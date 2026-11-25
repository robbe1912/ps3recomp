"""Checks the fast memory path of the PPU of PPU_FM3 1: the read accessors
without a label address, and the alias answer of the gate.

The fixture includes the production header and the production arm text of
ppu_loader.cpp, and links the production runtime/syscalls/spu_ea_alias.c. It
runs one process per mode. Each mode covers each access class for reads and
writes of each width (8, 16, 32, 64 bits): plain page, null page, IDENTITY
page, ALIAS page, MIXED page, EA bit 31, raw SPU problem-state register, GCM
ref-poll address, out of bounds, and each diagnostic bit.

The check builds three paths in two layouts (PPU_FM_INLINE_FAST 1 and 0):
the default (PPU_FM3 1), PPU_FM3 1 and PPU_FM3 0 (the path before this
change). The same source must pass in each build. The object of the default
path must hold no out-of-line copy of an accessor: each lifted read is inline.
Then each planted mutant must fail.
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
     "        if (PPU_FM_UNLIKELY(ea == ppu_hle_inject_base + 0x2008u)) "
     "return ppu_fm_read32_out(a);\n", ""),
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
    ("PPU_FM3 defaults to 0", "#  define PPU_FM3 1", "#  define PPU_FM3 0"),
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
#endif
#include <map>
#include <string>
#include "ps3emu/ppu_fastmem.h"
#include "arm.inc"

/* ---- authored guest memory: a 4 GiB reserve. commit() backs 64 KB at an EA. */
#define GUEST_BYTES 0x100010000ull
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
uint8_t*  vm_base;
uint32_t  ppu_vm_size = 0;
uint32_t  ppu_hle_inject_base = 0x7FE00000u;
volatile int g_spu_coh_armed = 0;
unsigned char g_spu_coh_bitmap[PPU_FM_COH_BITMAP_BYTES];
volatile unsigned char vm_read_sync_pages[1u << 20];
volatile uint32_t vm_read_sync_any = 0;
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

extern "C" {
uint32_t alias_fixup_impl(uint32_t ea);
void sysmem_alias_record(uint32_t addr, uint32_t size);
/* The production fixup, behind a counter. The alias file is compiled with
 * sysmem_spu_alias_fixup=alias_fixup_impl. */
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { fixups++; return alias_fixup_impl(ea); }
/* vm_read_sync_any stays 0, so the read sync never calls the dispatch. */
void vm_read_sync_dispatch(uint32_t address, uint32_t bytes) { printf("FAIL read_sync_dispatch\n"); exit(1); }
uint8_t  vm_read8 (uint64_t a) { family[0]++; last_family_addr = a; return 0x5Au; }
uint16_t vm_read16(uint64_t a) { family[1]++; last_family_addr = a; return 0x5AA5u; }
uint32_t vm_read32(uint64_t a) { family[2]++; last_family_addr = a; return 0x5AA5C35Au; }
uint64_t vm_read64(uint64_t a) { family[3]++; last_family_addr = a; return 0x5AA5C35A96967878ull; }
void vm_write8 (uint64_t a, uint8_t)  { family[4]++; last_family_addr = a; }
void vm_write16(uint64_t a, uint16_t) { family[5]++; last_family_addr = a; }
void vm_write32(uint64_t a, uint32_t) { family[6]++; last_family_addr = a; }
void vm_write64(uint64_t a, uint64_t) { family[7]++; last_family_addr = a; }
void ppu_fm_refpoll(void) { refpolls++; }
void ppu_fm_hotread8 (uint32_t ea, void* ra) { fires++; fire_ea = ea; fire_v = 0; fire_ra = ra; }
void ppu_fm_hotread16(uint32_t ea, void* ra) { fires++; fire_ea = ea; fire_v = 0; fire_ra = ra; }
void ppu_fm_hotread32(uint32_t ea, uint32_t v, void* ra) { fires++; fire_ea = ea; fire_v = v; fire_ra = ra; }
void ppu_fm_hotread64(uint32_t ea, uint64_t v, void* ra) { fires++; fire_ea = ea; fire_v = v; fire_ra = ra; }
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
    {"oob_window",   K_OOB,         0x4A000100u},
    {"oob_edge",     K_EDGE,        0x47FFFFFFu},
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
    fires = refpolls = fixups = 0;
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

/* True when the access leaves the gate for the out-of-line function. */
static int goes_out(int acc, const class_t* c)
{
    int read = acc < 4;
    int oob = ppu_vm_size != 0 && ppu_fm_oob(c->ea, (uint32_t)WIDTH[acc]);
    if (ppu_fm_slow & (read ? PPU_FM_S_READ : PPU_FM_S_WRITE)) return 1;
    switch (c->kind) {
    case K_PLAIN:
    case K_BIT31: return oob;
    case K_NULL:
    case K_RAW: return 1;                               /* the page table flags them */
    case K_REF: return read || oob;                     /* the page table flags it for reads */
    case K_OOB:
    case K_EDGE: return oob || (!PPU_FM3 && in_window(c->ea));
    default:
        /* Pages of the window. PPU_FM3 1 keeps IDENTITY and ALIAS pages
         * inline. MIXED pages and an access out of bounds go out. */
        if (!PPU_FM3) return 1;
        if (c->kind == K_MIXED_ALIAS || c->kind == K_MIXED_SELF) return 1;
        return ppu_fm_oob(c->ea, (uint32_t)WIDTH[acc]);
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
    for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; i++) {
        uint32_t got = sysmem_spu_alias_fixup(fix[i].ea);
        if (got != fix[i].want) return fail("fixup", got, fix[i].want);
    }
    if (sizeof sysmem_spu_alias_page != 0xB000u) return fail("alias_pages", sizeof sysmem_spu_alias_page, 0xB000u);
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
extern "C" FIXTURE_NOINLINE uint64_t spin_read##width(uint32_t ea)                    \
{                                                                                      \
    uint64_t last = 0;                                                                 \
    for (int i = 0; i < 200000; i++) last = ppu_fm_read##width(ea);                   \
    return last;                                                                       \
}                                                                                      \
extern "C" FIXTURE_NOINLINE uint32_t spin_read##width##_end(uint32_t ea) { return ea + 1u; }
#if defined(_MSC_VER) && !defined(__clang__)
#define FIXTURE_NOINLINE __declspec(noinline)
#else
#define FIXTURE_NOINLINE __attribute__((noinline))
#endif
SPIN(8) SPIN(16) SPIN(32) SPIN(64)

static int spin_at(int acc, uint32_t ea, uint32_t fire_ea_want)
{
    const char* names[4] = { "read8", "read16", "read32", "read64" };
    char what[64];
    uint64_t (*fn[4])(uint32_t) = { spin_read8, spin_read16, spin_read32, spin_read64 };
    void* start[4] = { (void*)spin_read8, (void*)spin_read16, (void*)spin_read32, (void*)spin_read64 };
    void* end[4] = { (void*)spin_read8_end, (void*)spin_read16_end, (void*)spin_read32_end,
                     (void*)spin_read64_end };
    uint64_t raw[4] = { 0, 0, vm_base[fire_ea_want] | vm_base[fire_ea_want + 1] << 8
                        | vm_base[fire_ea_want + 2] << 16 | (uint64_t)vm_base[fire_ea_want + 3] << 24,
                        (uint64_t)vm_base[fire_ea_want] | (uint64_t)vm_base[fire_ea_want + 1] << 8
                        | (uint64_t)vm_base[fire_ea_want + 2] << 16 | (uint64_t)vm_base[fire_ea_want + 3] << 24
                        | (uint64_t)vm_base[fire_ea_want + 4] << 32 | (uint64_t)vm_base[fire_ea_want + 5] << 40
                        | (uint64_t)vm_base[fire_ea_want + 6] << 48 | (uint64_t)vm_base[fire_ea_want + 7] << 56 };
    snprintf(what, sizeof what, "spin_%s", names[acc]);
    fires = 0; fire_ra = 0;
    (void)fn[acc](ea);
    if (fires != 1) return fail(what, fires, 1);
    if (fire_ea != fire_ea_want) return fail(what, fire_ea, fire_ea_want);
    if (fire_v != raw[acc]) return fail(what, fire_v, raw[acc]);
    if (!(fire_ra >= start[acc] && fire_ra < end[acc])) return fail(what, (unsigned long long)(uintptr_t)fire_ra, (unsigned long long)(uintptr_t)start[acc]);
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

/* With the count bit set, each access of a window page leaves the fast path
 * and is counted: the counter reads the clock once per 16384 hits. A plain
 * page stays inline and is not counted. */
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
    if (now_calls != 8) return fail("count_plain", now_calls, 8);
    return 0;
}

static void set_line(uint32_t ea, int on)
{
    uint32_t line = ea >> 7;
    if (on) g_spu_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    else    g_spu_coh_bitmap[line >> 3] &= (unsigned char)~(1u << (line & 7));
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
        environment[!strcmp(mode, "kill") ? "PPU_FM" : "PPU_RWATCH"] = "1";
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
    printf("FAIL mode %s unknown\n", mode);
    return 1;
}
'''

MODES = ("unarmed", "all", "count", "diag", "kill", "watch", "bounds", "spin", "spinoff", "coh")
LAYOUTS = (1, 0)     # PPU_FM_INLINE_FAST: inline at each call site, then one copy per unit
PATHS = (None, 1, 0)   # PPU_FM3: the default, 1, then the path before this change
WIDTHS = ("8", "16", "32", "64")


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


def build(environment, work, cc, msvc_style, header, arm, alias, vendor, fm3, inline_fast):
    """Writes the header, the arm text and the alias file into `work`, then
    builds `fixture` there. fm3: None (the default of the header), 1 or 0."""
    work.mkdir(parents=True, exist_ok=True)
    target = work / "mut" / FASTMEM
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(header)
    (work / "fixture.cpp").write_text(FIXTURE)
    (work / "arm.inc").write_text(arm)
    (work / "alias.c").write_text(alias)
    directories = [work / "mut" / "include", vendor / "include", vendor / "runtime" / "spu"]
    defines = ["PPU_FM_INLINE_FAST=%d" % inline_fast] + ([] if fm3 is None else ["PPU_FM3=%d" % fm3])
    if msvc_style:
        common = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything"]
        common += ["/D" + d for d in defines] + ["/I" + p for p in map(str, directories)]
        fixture = common + ["/c", "fixture.cpp", "/Fofixture.obj"]
        alias = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything",
                 "/Dsysmem_spu_alias_fixup=alias_fixup_impl"] \
            + ["/I" + p for p in map(str, directories)] + ["/c", "alias.c", "/Foalias.obj"]
        link = [cc, "/nologo", "fixture.obj", "alias.obj", "/Fefixture" + EXE]
    else:
        # The fixture is C++. clang, cc and gcc need their C++ drivers.
        driver = {"clang": "clang++", "cc": "c++", "gcc": "g++"}.get(Path(cc).name.lower(), cc)
        options = ["-std=c++17", "-O2", "-UNDEBUG", "-Wno-everything"]
        common = [driver] + options + ["-D" + d for d in defines] + ["-I" + p for p in map(str, directories)]
        fixture = common + ["-c", "fixture.cpp", "-o", "fixture.o"]
        alias = [cc, "-O2", "-Wno-everything", "-Dsysmem_spu_alias_fixup=alias_fixup_impl"] \
            + ["-I" + p for p in map(str, directories)] + ["-c", "alias.c", "-o", "alias.o"]
        link = [driver] + ["fixture.o", "alias.o", "-o", "fixture" + EXE]
    run_command(environment, work, fixture)
    run_command(environment, work, alias)
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


def expected_copies(fm3, inline_fast):
    if inline_fast == 0:
        return sorted((kind, width) for kind in ("read", "write") for width in WIDTHS)
    if fm3 == 0:
        return sorted(("read", width) for width in WIDTHS)
    return []


def verify(work):
    for mode in MODES:
        result = subprocess.run(QUIET + [str(work / ("fixture" + EXE)), mode], capture_output=True,
                                text=True, errors="replace", timeout=240)
        assert result.returncode == 0 and not result.stdout, \
            (mode, result.returncode, result.stdout.strip()[-700:], result.stderr.strip()[-300:])


def reason(error):
    """The first line of a rejection: the mode and the FAIL line, or a build error."""
    text = str(error)
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in lines:
        if "FAIL" in line or "error" in line:
            return line[:150]
    return (lines[0] if lines else text)[:150]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor", type=Path, default=VENDOR)
    args = parser.parse_args()
    header = (args.vendor / FASTMEM).read_text()
    loader = (args.vendor / LOADER).read_text()
    alias = (args.vendor / ALIAS).read_text()
    arm = arm_text(loader)
    for name, anchor, *_ in HEADER_MUTANTS:
        assert header.count(anchor) >= 1, (name, anchor)
    for name, anchor, *_ in ALIAS_MUTANTS:
        assert alias.count(anchor) == 1, (name, anchor)
    for name, anchor, *_ in ARM_MUTANTS:
        assert arm.count(anchor) == 1, (name, anchor)
    assert FIXTURE.count("#include \"ps3emu/ppu_fastmem.h\"") == 1
    assert FIXTURE.count('"arm.inc"') == 1

    if GAME:
        environment = project.build_environment()
    else:
        environment = dict(os.environ)
    cc = environment.get("MV_CLANG") or compiler()
    msvc_style = Path(cc).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")

    def make(root, fm3, inline_fast, header_text, arm_text_, alias_text):
        copies = build(environment, root, cc, msvc_style, header_text, arm_text_, alias_text,
                       args.vendor, fm3, inline_fast)
        assert copies == expected_copies(fm3, inline_fast), \
            "out-of-line accessors %s, want %s" % (copies, expected_copies(fm3, inline_fast))
        verify(root)

    rejected = []
    with tempfile.TemporaryDirectory(prefix="ppu-fastmem3-") as directory:
        root = Path(directory)
        for fm3 in PATHS:
            for inline_fast in LAYOUTS:
                make(root / "actual", fm3, inline_fast, header, arm, alias)
        groups = (("header", HEADER_MUTANTS, (header, arm, alias)),
                  ("alias", ALIAS_MUTANTS, (header, arm, alias)),
                  ("arm", ARM_MUTANTS, (header, arm, alias)))
        for group, mutants, texts in groups:
            for mutant in mutants:
                name, anchor, replacement = mutant[:3]
                layouts = mutant[3] if len(mutant) > 3 else (1,)
                parts = list(texts)
                position = {"header": 0, "alias": 2, "arm": 1}[group]
                changed = parts[position].replace(anchor, replacement)
                assert changed != parts[position], name
                parts[position] = changed
                for inline_fast in layouts:
                    try:
                        make(root / "mutant", None, inline_fast, parts[0], parts[1], parts[2])
                    except AssertionError as error:
                        rejected.append(name)
                        print("rejected: %s (layout %d): %s" % (name, inline_fast, reason(error)))
                    else:
                        raise AssertionError("mutant accepted: " + name)
        count = len(HEADER_MUTANTS) + len(ALIAS_MUTANTS) + len(ARM_MUTANTS)
    print("PASS: PPU_FM3 fast memory path, %d paths x %d layouts, %d mutants rejected"
          % (len(PATHS), len(LAYOUTS), count))


if __name__ == "__main__":
    main()
