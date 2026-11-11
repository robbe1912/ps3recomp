#!/usr/bin/env python3
"""Jump-table vectors for the vendored PPU lifter: lwax tables and spilled bases.

No SDK, no ELF, no game bytes: the PowerPC words are hand-assembled and memory
is a dict. Every word is round-tripped through ppu_disasm.decode first, so an
encoding mistake reports as ENCODING and never as a false lifter failure.

The defect these pin: a switch whose table base is loaded (one- or two-level
from the TOC), stored to a stack slot, reloaded at the dispatch and then reused
for the case address was dropped by discover_jump_tables, so its bctr lifted to
an indirect tail call that skipped the epilogue. MotorStorm's Havok solver
switch in hkExportImpulsesAndRhs has exactly this shape; a hard collision takes
it and the physics worker then runs on with clobbered callee-saved registers
(#155). The same detector also only accepted lwzx as the indexed table load.

Run: python scripts/check_ppu_spilled_jumptables.py
"""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import ppu_disasm                                   # noqa: E402
from ppu_lifter import discover_jump_tables         # noqa: E402

TEXT_LO, TEXT_HI = 0x10000, 0x20000
TOC = 0x100000


def w_cmplwi(bf, ra, ui):  return (10 << 26) | (bf << 23) | (ra << 16) | (ui & 0xFFFF)
def w_lwz(rt, ra, d):      return (32 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)
def w_lwzx(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (23 << 1)
def w_lwax(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (341 << 1)
def w_add(rt, ra, rb):     return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (266 << 1)
def w_slwi(ra, rs, n):     return (21 << 26) | (rs << 21) | (ra << 16) | (n << 11) | (0 << 6) | ((31 - n) << 1)
def w_bctr():              return (19 << 26) | (20 << 21) | (528 << 1)
def w_ld(rt, ra, d):       return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_stw(rs, ra, d):      return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def w_stb(rs, ra, d):      return (38 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def w_std(rs, ra, d):      return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_stdu(rs, ra, d):     return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC) | 1
def w_li(rt, si):          return (14 << 26) | (rt << 21) | (si & 0xFFFF)


def w_mtctr(rs):
    spr = ((9 & 0x1F) << 5) | ((9 >> 5) & 0x1F)     # CTR is SPR 9, halves swapped
    return (31 << 26) | (rs << 21) | (spr << 11) | (467 << 1)


FAILS = []


def check(name, got, want):
    if got == want:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name}: got {got if got is None else [hex(x) for x in got]}, "
              f"want {want if want is None else [hex(x) for x in want]}")
        FAILS.append(name)


def discover(words, mem):
    insns = [ppu_disasm.decode(w, TEXT_LO + 4 * i) for i, w in enumerate(words)]
    for i, (w, ins) in enumerate(zip(words, insns)):
        if ins is None or ins.mnemonic in ('.long', 'unknown'):
            print(f"ENCODING word {i} {w:08X} -> {ins}")
            FAILS.append(f'encoding {i}')
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI, func_starts=[TEXT_LO])
    return tables.get(insns[-1].addr)                # the bctr is always last


def offset_table(table, *offsets):
    mem = {table + 4 * i: o & 0xFFFFFFFF for i, o in enumerate(offsets)}
    mem[table + 4 * len(offsets)] = 0xFFFFFFFF      # misaligned target: the table ends
    return mem


def main():
    table = 0x11000

    # lwax as the indexed load (signed offsets; one case sits below the table)
    mem = {TOC + 16: table, **offset_table(table, 0x100, -0x200, 0x300)}
    check("offset table loaded with lwax",
          discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwax(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem),
          [0x10E00, 0x11100, 0x11300])

    # one-level base, spilled and reloaded, base register reused for the target
    mem = {TOC + 16: table, **offset_table(table, 0x100, 0x200, 0x300)}
    check("spilled one-level base reused for the case address",
          discover([w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_li(5, 0), w_cmplwi(0, 3, 2),
                    w_ld(11, 1, 0x2D0), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
                    w_add(11, 0, 11), w_mtctr(11), w_bctr()], mem),
          [0x11100, 0x11200, 0x11300])

    # the MotorStorm shape: two-level base, spilled, reloaded, reused
    #   lwz r30,-0x14F0(r2); lwz r5,-0x8000(r30); std r5,-0xC8(r1) ...
    #   ld r6,-0xC8(r1); lwzx r0,r9,r6; add r6,r0,r6; mtctr r6; bctr
    mid = 0x208000
    mem = {TOC - 0x14F0: mid, mid - 0x8000: table, **offset_table(table, 0x100, 0x200, 0x300, 0x400)}
    check("spilled two-level base reused for the case address",
          discover([w_lwz(30, 2, -0x14F0), w_lwz(5, 30, -0x8000), w_std(5, 1, -0xC8),
                    w_li(7, 1), w_cmplwi(0, 3, 3), w_ld(6, 1, -0xC8), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 6), w_add(6, 0, 6), w_mtctr(6), w_bctr()], mem),
          [0x11100, 0x11200, 0x11300, 0x11400])

    # negative: a reload of a slot nothing stored stays unresolved
    mem = {TOC + 16: table, **offset_table(table, 0x100, 0x200)}
    check("reload of an unstored slot stays unresolved",
          discover([w_lwz(10, 2, 16), w_std(10, 1, 0x2D8), w_cmplwi(0, 3, 1),
                    w_ld(11, 1, 0x2D0), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
                    w_add(11, 0, 11), w_mtctr(11), w_bctr()], mem),
          None)

    # negative: the frame moved between the store and the reload
    check("store before a frame change does not resolve the reload",
          discover([w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_stdu(1, 1, -0x80),
                    w_cmplwi(0, 3, 1), w_ld(11, 1, 0x2D0), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 11), w_add(11, 0, 11), w_mtctr(11), w_bctr()], mem),
          None)

    # Every intervening write is considered by byte extent, including the low
    # word of an eight-byte spill and byte writes at either boundary.
    mem = {TOC + 16: table, **offset_table(table, 0x100, 0x200, 0x300)}
    for name, store in [('low word', w_stw(0, 1, 0x2D4)),
                        ('last byte', w_stb(0, 1, 0x2D7)),
                        ('overlapping doubleword', w_std(0, 1, 0x2CC))]:
        check('overwritten spill ' + name,
              discover([w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_li(0, 0),
                        store, w_cmplwi(0, 3, 2), w_ld(11, 1, 0x2D0),
                        w_slwi(9, 3, 2), w_lwzx(0, 9, 11), w_add(11, 0, 11),
                        w_mtctr(11), w_bctr()], mem), None)
    check('narrow store cannot define a wide reload',
          discover([w_lwz(10, 2, 16), w_stw(10, 1, 0x2D0), w_cmplwi(0, 3, 2),
                    w_ld(11, 1, 0x2D0), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
                    w_add(11, 0, 11), w_mtctr(11), w_bctr()], mem), None)
    check('adjacent store preserves spill',
          discover([w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_stw(0, 1, 0x2D8),
                    w_cmplwi(0, 3, 2), w_ld(11, 1, 0x2D0), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 11), w_add(11, 0, 11), w_mtctr(11), w_bctr()], mem),
          [0x11100, 0x11200, 0x11300])

    # unchanged: the plain gcc offset idiom
    mem = {TOC + 16: table, **offset_table(table, 0x100, 0x200, 0x300)}
    check("plain offset idiom unchanged",
          discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem),
          [0x11100, 0x11200, 0x11300])

    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("PASS: lwax and spilled one/two-level jump-table bases, with negative controls")
    return 0


if __name__ == '__main__':
    sys.exit(main())
