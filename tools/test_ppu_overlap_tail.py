#!/usr/bin/env python3
"""Unrelated function entries cannot change an overlapping tail's meaning."""
import struct

from ppu_disasm import disassemble_bytes
from ppu_lifter import LiftedFunction, PPULifter


def main():
    words = [0x60000000] * 64
    # Observable instructions at different enclosing ends distinguish tails.
    words[0x60//4] = 0x38630001
    words[0x80//4] = 0x38630002
    insns = disassemble_bytes(struct.pack('>'+'I'*len(words), *words), 0x1000, True)
    cases = [
        ([(0x1000, 0x10C0), (0x1020, 0x10A0), (0x1040, 0x1080)], 0x1050, 0x1080),
        # A nearer interval has ended; an earlier enclosing interval survives.
        ([(0x1000, 0x10C0), (0x1020, 0x1040), (0x1040, 0x1080)], 0x1090, 0x10C0),
        # Equal starts preserve the widest extent, as emission deduplication does.
        ([(0x1000, 0x10C0), (0x1040, 0x1060), (0x1040, 0x1080)], 0x1050, 0x1080),
        ([(0x1000, 0x1020), (0x1040, 0x1060)], 0x1050, 0x1060),
    ]
    extras = [[], [(0x800, 0x804)], [(0x1800, 0x1804)],
              [(0x800, 0x804), (0x1800, 0x1804)], [(0x1048, 0x104C)]]
    failed = total = 0
    for bounds, target, expected_end in cases:
        reference = PPULifter().lift_function(insns, target, expected_end)
        for extra in extras:
            entries = bounds + extra
            for ordered in (entries, entries[::-1], entries[1:] + entries[:1]):
                lifter = PPULifter()
                lifter.code_hi = 0x2000
                lifter.functions = [LiftedFunction(f'f_{a:x}_{b:x}', a, b, [])
                                    for a, b in ordered]
                lifter.call_targets = {target}
                lifter.generate_mid_function_entries(insns)
                actual = next(f for f in lifter.functions if f.start_addr == target)
                total += 1
                if (actual.end_addr != expected_end or
                        actual.body_lines != reference.body_lines or
                        actual.fallthrough_to != reference.fallthrough_to):
                    failed += 1
                    print(f'target={target:x} expected_end={expected_end:x} '
                          f'actual_end={actual.end_addr:x} extra={extra}')
    print(f'{total} decoded/lifted overlap cases, {failed} failed')
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())
