# SPU reserved-line bitmap test

`spu_coh_bitmap_test(addr)` is an inline function in `runtime/spu/spu_coherency.h`.
It reports whether the 128-byte line of guest address `addr` holds an SPU reservation.
`spu_coh_reserve` sets one bit per line in `g_spu_coh_bitmap` at the first GETLLAR of that line.
No code clears a bit.

## Rules for callers

- Test the armed flag first. The armed flag is zero until the first GETLLAR.
  A caller that skips this test reads the bitmap before any SPU ran.
- `spu_coh_is_reserved` is the armed test plus the inline test. Use it where the
  armed flag is not in scope.
- Do not read `g_spu_coh_bitmap` in another way. One bit is one line, counted
  from the low end of each byte.

## Layout

| Item | Value |
|---|---|
| Line size | 128 bytes (`SPU_COH_LINE_SHIFT` = 7) |
| Lines | 2^25 |
| Bitmap size | 4 MiB |
| Byte index | `line >> 3` |
| Bit index | `line & 7` |

## Check

Run `python tests/check_spu_coh_bitmap.py`.

The check compiles the production header and the production source with the
fixture `tests/spu_coh_bitmap_harness.c`. The fixture asserts the result for
each byte of a reserved line, for the two neighbour lines, and for the top
line of the 32-bit address space. It rejects six mutants:

- line shift 7 to 6
- line shift 7 to 8
- byte index `>> 3` to `>> 2`
- bit mask `& 7` to `& 3`
- bit order from the high end of the byte
- armed check removed from `spu_coh_is_reserved`

The check needs a C compiler. On Windows it uses `MV_CLANG` or `CC`. Without
them it looks for `clang-cl`, `clang` or `cc`.
