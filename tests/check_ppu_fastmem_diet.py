#!/usr/bin/env python3
"""Game-free checks for the lean fast memory paths.

1. Compiles the production ppu_fastmem.h with authored stubs. Assertions: the
   spin detector fires once in 200000 same-address reads while its switch is on
   and never while it is off; a store into a reserved line takes the slow path;
   a store into a free line writes memory; an armed ppu_fm_diag sends reads and
   writes to the slow path. Mutants of the header must fail.
2. Compiles the production bitmap test of spu_coherency.h next to the copy in
   ppu_fastmem.h. Both must agree on every tested address. Mutants of the copy
   must fail.
3. Compiles the ppu_fm_arm text of the production ppu_loader.cpp. Only the
   exact value PPU_FM_SPINDET=0 turns the switch off. Mutants must fail.
No game input; no performance claim."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

VENDOR = ROOT
FASTMEM = Path("include/ps3emu/ppu_fastmem.h")
LOADER = Path("runtime/ppu/ppu_loader.cpp")
EXE = ".exe" if os.name == "nt" else ""
ARM_BEGIN = 'extern "C" volatile uint32_t ppu_fm_diag = 0;'
ARM_END = "int is_fm = (knobs[i][0] == 'P'"
FLAG_TEST = "if (PPU_FM_LIKELY(!ppu_fm_spin_det)) break;"
FIRE = "if (PPU_FM_UNLIKELY(++fm_n == 200000)) { fire_args; fm_n = 0; }"
STORE = "if (PPU_FM_UNLIKELY(g_spu_coh_armed && spu_coh_bitmap_test(ea))) {"
BIT_TEST = "return (g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u;"
LINE_SHIFT = "#define PPU_FM_COH_LINE_SHIFT   7"
PARSE = "if (spindet && spindet[0] == '0' && !spindet[1]) ppu_fm_spin_det = 0;"

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
    ("read test removed", "if (PPU_FM_UNLIKELY(ppu_fm_diag))       return vm_read32(a);",
     "if (PPU_FM_UNLIKELY(0))       return vm_read32(a);"),
    ("write test removed", "if (PPU_FM_UNLIKELY(ppu_fm_diag || g_barrier_sync_watch)) { vm_write32(a, v); return; }",
     "if (PPU_FM_UNLIKELY(g_barrier_sync_watch)) { vm_write32(a, v); return; }"),
]
LOADER_MUTANTS = [
    ("parse accepts 00", PARSE, "if (spindet && spindet[0] == '0') ppu_fm_spin_det = 0;"),
    ("parse removed", PARSE, ""),
    ("parse accepts a leading zero word", PARSE, PARSE.replace("!spindet[1]", "spindet[1] != 'x'")),
    ("default off", 'extern "C" volatile uint32_t ppu_fm_spin_det = 1;',
     'extern "C" volatile uint32_t ppu_fm_spin_det = 0;'),
]

READ_SYNC = r'''
#ifndef VM_READ_SYNC_STUB_H
#define VM_READ_SYNC_STUB_H
#include <stdint.h>
static inline void vm_cpu_read_sync(uint64_t address, unsigned bytes)
{ (void)address; (void)bytes; }
#endif
'''

SPU_RAW = r'''
#ifndef SPU_RAW_STUB_H
#define SPU_RAW_STUB_H
#include <stdint.h>
static inline int spu_raw_is_reg(uint32_t ea) { (void)ea; return 0; }
#endif
'''

# The runtime state and the slow paths that ppu_fastmem.h calls.
STUBS = r'''
/* ---- authored stubs of the runtime state ---- */
#define MEM_BYTES 0x400000u
#define EA        0x123456u  /* an index into memory */
#define VM_BYTES   0x7FE02010u   /* covers the GCM control block */
static unsigned char* memory;     /* guest window, allocated in reset() */
uint8_t*  vm_base;
uint32_t  ppu_vm_size = 0;
uint32_t  ppu_hle_inject_base = 0x7FE00000u;
volatile uint32_t g_barrier_sync_watch = 0;
volatile int g_spu_coh_armed = 0;
unsigned char g_spu_coh_bitmap[PPU_FM_COH_BITMAP_BYTES];
static int slow_reads, slow_writes, fires, refpolls;
static std::map<std::string, std::string> environment;

extern "C" {
uint32_t sysmem_spu_alias_fixup(uint32_t ea) { return ea; }
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

static void reset(void)
{
    slow_reads = slow_writes = fires = refpolls = 0;
    ppu_fm_diag = 0; ppu_fm_spin_det = 1; g_spu_coh_armed = 0;
    memset(g_spu_coh_bitmap, 0, PPU_FM_COH_BITMAP_BYTES);
    if (!memory) memory = (unsigned char*)calloc(VM_BYTES, 1);
    vm_base = memory;
    memset(memory, 0, MEM_BYTES);
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

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    const uint32_t initial_spin_det = ppu_fm_spin_det;   /* the definition, before any test */
    reset();
    if (!strcmp(mode, "spin_on")) {
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
    } else if (!strcmp(mode, "spin_off")) {
        ppu_fm_spin_det = 0;
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
        ppu_fm_diag = 1;
        if (ppu_fm_read32(EA) != 0x5A5A5A5Au || slow_reads != 1) return fail("diag_reads", slow_reads, 1);
        (void)ppu_fm_read8(EA); (void)ppu_fm_read16(EA); (void)ppu_fm_read64(EA);
        if (slow_reads != 4) return fail("diag_reads_other", slow_reads, 4);
        if (fires) return fail("diag_fires", fires, 0);
        ppu_fm_write8(EA, 1); ppu_fm_write16(EA, 1); ppu_fm_write32(EA, 1); ppu_fm_write64(EA, 1);
        if (slow_writes != 4) return fail("diag_writes", slow_writes, 4);
        if (memory[EA]) return fail("diag_wrote_memory", memory[EA], 0);
        reset(); g_barrier_sync_watch = 1; ppu_fm_write32(EA, 1);
        if (slow_writes != 1 || memory[EA]) return fail("barrier_watch_writes", slow_writes, 1);
        if (refpolls) return fail("refpolls", refpolls, 0);
    } else if (!strcmp(mode, "refpoll")) {
        (void)ppu_fm_read32(ppu_hle_inject_base + 0x2008u);
        if (refpolls != 1) return fail("refpolls", refpolls, 1);
        (void)ppu_fm_read32(ppu_hle_inject_base + 0x200Cu);
        if (refpolls != 1) return fail("refpolls_other", refpolls, 1);
    } else if (!strcmp(mode, "arm")) {
        const char* unset = 0;
        if (initial_spin_det != 1) return fail("default", (int)initial_spin_det, 1);
        const char* values[] = {"0", "1", "00", "0 ", " 0", "false", "", "10", "0x0", "0.0", unset};
        for (int i = 0; i < 10; i++) {
            ppu_fm_diag = 0; ppu_fm_spin_det = 1;
            environment.clear();
            if (values[i]) environment["PPU_FM_SPINDET"] = values[i];
            ppu_fm_arm();
            if (ppu_fm_spin_det != (i == 0 ? 0u : 1u)) return fail("switch", i, ppu_fm_spin_det);
            if (ppu_fm_diag) return fail("diag", ppu_fm_diag, 0);
        }
        /* PPU_FM=0 is the kill switch and does not move the spin switch. */
        environment.clear(); environment["PPU_FM"] = "0";
        ppu_fm_arm();
        if (ppu_fm_diag != 1 || ppu_fm_spin_det != 1) return fail("kill_switch", ppu_fm_diag, 1);
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


def build(environment, work, cc, msvc_style, header, arm, vendor):
    """Writes the header, the stub headers and the sources into `work`, then
    builds `fixture` and `agree` there."""
    work.mkdir(parents=True, exist_ok=True)
    for path, text in ((FASTMEM, header), (Path("include/ps3emu/guest_read_sync.h"), READ_SYNC),
                       (Path("runtime/spu/spu_raw.h"), SPU_RAW)):
        target = work / "mut" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    for name, text in (("fixture.cpp", FIXTURE), ("coherent.cpp", COHERENT), ("agree.cpp", AGREE)):
        (work / name).write_text(text)
    (work / "arm.inc").write_text(arm)
    directories = [work / "mut" / "include", vendor / "include", vendor / "runtime" / "spu"]
    if msvc_style:
        common = [cc, "/nologo", "/O2", "/UNDEBUG", "/clang:-Wno-everything"]
        common += ["/I" + str(path) for path in directories] + ["/Fo" + str(work) + os.sep]
        fixture = [str(work / "fixture.cpp"), "/Fe" + str(work / ("fixture" + EXE))]
        agree = [str(work / "coherent.cpp"), str(work / "agree.cpp"),
                 "/Fe" + str(work / ("agree" + EXE))]
    else:
        # The sources are C++. clang, cc and gcc need their C++ drivers.
        driver = {"clang": "clang++", "cc": "c++", "gcc": "g++"}.get(Path(cc).name.lower(), cc)
        common = [driver, "-std=c++17", "-O2", "-UNDEBUG", "-Wno-everything"]
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
    for mode in ("spin_on", "spin_off", "store", "diag", "refpoll", "arm"):
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
    cc = compiler()
    msvc_style = Path(cc).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix="ppu-fastmem-diet-") as directory:
        root = Path(directory)
        verify(build(environment, root / "actual", cc, msvc_style, header, arm, args.vendor))
        rejected = 0
        for name, anchor, replacement in HEADER_MUTANTS:
            try:
                verify(build(environment, root / "header", cc, msvc_style,
                             header.replace(anchor, replacement), arm, args.vendor))
            except AssertionError:
                rejected += 1
            else:
                raise AssertionError("header mutant accepted: " + name)
        for name, anchor, replacement in LOADER_MUTANTS:
            try:
                verify(build(environment, root / "loader", cc, msvc_style, header,
                             arm.replace(anchor, replacement), args.vendor))
            except AssertionError:
                rejected += 1
            else:
                raise AssertionError("loader mutant accepted: " + name)
    print("PASS: fast memory paths, %d header mutants and %d loader mutants rejected"
          % (len(HEADER_MUTANTS), len(LOADER_MUTANTS)))


if __name__ == "__main__":
    main()
