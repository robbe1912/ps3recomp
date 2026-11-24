# Fast memory paths

`include/ps3emu/ppu_fastmem.h` holds lean paths for the guest loads and stores
of the lifted code. The fork has them from the game tree of MotorStorm (change
#348 of that tree), with the `ppu_fm_*` block of `runtime/ppu/ppu_loader.cpp`.
The game tree is the source of the measured results. This fork is the source of
the code.

## State of the fork

- A lifted unit uses the paths only when its `ppu_recomp.h` includes the
  header. The fork lifter (`tools/ppu_lifter.py`) does not emit that include.
  It emits its own inline loads (`vm_read*_inl`, gate `vm_inl_ok`) and calls
  the out-of-line `vm_write*`. The game lifter emits the include. Open item:
  move that emission into the fork lifter.
- `ppu_hle_init()` calls `ppu_fm_arm()` (`runtime/ppu/ppu_hle.cpp`). A host
  that does not call it keeps each bit of `ppu_fm_slow` set: each access
  takes the out-of-line function, then the out-of-line family.
- The out-of-line family of the fork (`vm_read*`, `vm_write*` in
  `runtime/ppu/ppu_loader.cpp`) has its own detectors and prints. It does not
  call the fire halves `ppu_fm_hotread*`. The game family calls them.
- `runtime/ppu/ppu_loader.cpp` defines `g_barrier_sync_watch` as `volatile`,
  as the game tree does. The header does not declare that word now: the
  paths read `PPU_FM_S_WATCH` instead.

## Rules for callers

- Include the header, then rename the `vm_read*` and `vm_write*` names to the
  `ppu_fm_*` accessors. The rename is behind `#ifndef PPU_FASTMEM_NO_RENAME`.
  `runtime/ppu/ppu_loader.cpp` defines the out-of-line family, so it must
  define `PPU_FASTMEM_NO_RENAME`.
- Call `ppu_hle_init()` before any guest code runs. `ppu_fm_arm()` is the
  only writer of `ppu_fm_slow`, of `ppu_fm_kind` and of `ppu_fm_spin_det`.
- `ppu_fm_arm()` reads `ppu_vm_size` and `ppu_hle_inject_base`. Call it
  after the host set them, and again after each change of one of them.
- `PPU_FM` with a value that starts with `0` routes every access through the
  out-of-line family. It is the kill switch.
- `PPU_FM_SPINDET=0` turns the hot-spin detector off. Only that exact value
  does.
- `SPU_ATOM_EA` with an address routes each store through the family, where
  `g_barrier_sync_watch` is in scope. Reads keep the fast path.
- `PPU_FM_COUNT` in the environment turns the call counter on. Only the
  exact value `0` keeps it off.
- `PPU_FM_INLINE_FAST=0` before the include puts one out-of-line copy of
  each accessor in the unit. The default `1` puts the gate and the fast body
  at each call site.

## Layout

Each accessor has three parts.

- The gate (`ppu_fm_readN`, `ppu_fm_writeN`): one load of the flag word
  `ppu_fm_slow` and one load of one byte of the page table `ppu_fm_kind`.
  A set bit or a set page bit sends the access to the out-of-line function.
- The out-of-line function (`ppu_fm_readN_out`, `ppu_fm_writeN_out`), one
  copy per unit. A set flag bit sends the whole access to the out-of-line
  family, so each knob behaves as in a build without the paths. A set page
  bit gets the exact tests. Each test that is true sends the whole access to
  the family.
- The fast body (`ppu_fm_readN_body`, `ppu_fm_writeN_body`), always inline.

Flag word:

| Bit | Value | Set by | Effect |
|---|---|---|---|
| `PPU_FM_S_DIAG` | 1 | a knob of the list of `ppu_fm_arm`, or `PPU_FM=0` | reads and writes go to the family |
| `PPU_FM_S_WATCH` | 2 | `SPU_ATOM_EA` with an address | writes go to the family |
| `PPU_FM_S_COUNT` | 4 | `PPU_FM_COUNT` | reads and writes go to the family; the out-of-line function counts the call |

The initial value is `PPU_FM_S_ALL` (7).

Page table, one byte per 4 KB guest page, filled by `ppu_fm_arm()`:

| Pages | Bit | Reason |
|---|---|---|
| each page that is not entirely inside `ppu_vm_size` | read, write | bounds report |
| `0x00000` to `0x0000F` | read | null-read report |
| `0x00000` | write | null-store report |
| `0x45000` to `0x4FFFF` | read, write | alias window, then `sysmem_spu_alias_fixup` |
| pages `0x040` to `0x0FF` of each of the 6 raw SPU slots at `0xE0000000` | read, write | register side effects |
| the page of `ppu_hle_inject_base + 0x2008` | read | GCM ref-poll |

Exact tests of the out-of-line function, in order:

| Accessor | Tests |
|---|---|
| `ppu_fm_read8_out`, `read64_out` | alias fixup, flag word, bounds, null page |
| `ppu_fm_read16_out` | alias fixup, flag word, bounds |
| `ppu_fm_read32_out` | alias fixup, flag word, bounds, null page, raw SPU registers, GCM ref-poll address |
| `ppu_fm_write8_out`, `write16_out`, `write64_out` | alias fixup, flag word, bounds, null page |
| `ppu_fm_write32_out` | alias fixup, flag word, bounds, null page, raw SPU registers |

- The page table only selects the route. An address on a flagged page that
  fails each exact test gets the fast body in the out-of-line function.
- The read body holds the read sync, the load, the byte swap and the hot-spin
  detector. `vm_cpu_read_sync` keeps its behavior.
- The detector counts the consecutive reads of one address through one
  read width. At 200000 it calls `ppu_fm_hotreadN` one time and starts again.
  The state is one TLS object per read width.
- The store body tests the reserved-line bitmap. A reserved line goes to
  `vm_writeN` with the original address, as before.
- With `PPU_FM_INLINE_FAST 1` the fire helpers get the address of a label at
  the call site as caller address. It lies in the same guest function as the
  return address of a call.

## Counter

`PPU_FM_COUNT` prints lines to stderr. `ppu_fm_arm()` prints
`[PPU_FM_COUNT] on` one time. Then each second with calls has one line for
each thread with calls, then one line with the sum:

```
[PPU_FM_COUNT] seq=7 scope=thread slot=0 tid=4124 guest=3 read8=12 read16=0 read32=8061 read64=40 write8=0 write16=8 write32=731 write64=0 calls=8152
[PPU_FM_COUNT] seq=7 scope=all start=90123456789 end=91123456789 frequency=10000000 read8=12 read16=0 read32=8061 read64=40 write8=0 write16=8 write32=731 write64=0 calls=8152
```

- The numbers are calls of the accessors, not guest instructions.
- `tid` is the host thread id. The `[thr]` lines of the log give its name.
  `guest` is the guest thread id of the context of the thread, or 0.
- The first 31 threads have a slot of their own. All other threads share the
  last slot.
- A thread reads the clock at each 16384th call of that thread. The first
  thread that sees one second or more since the last line prints the lines.
  A second without calls has no line.
- `start` and `end` are clock ticks. The clock is `QueryPerformanceCounter`
  on Windows and `CLOCK_MONOTONIC` elsewhere.
- The fast path of each accessor has no code of the counter.

## Check

```
python tests/check_ppu_fastmem_diet.py
```

The script needs clang-cl on Windows, or clang, gcc or cc with their C++
drivers elsewhere. Set `MV_CLANG` or `CC` to select a compiler. It compiles
the production header, the production `ppu_fm_arm` text with the page table,
the flag word and the counter of the production `ppu_loader.cpp`, and the
production `spu_raw.h`, with authored stubs and an authored clock. Each
header build runs in both layouts, `PPU_FM_INLINE_FAST` 1 and 0. No game
input. No performance claim.

Mutants that the script rejects:

- header (55): detector switch test removed or inverted; threshold changed;
  equal changed to not equal; armed test removed; bitmap test inverted; line
  shift changed; bit mask changed; byte index changed; the gate without the
  flag word; the gate without the page table; stores with the read mask;
  reads with the write mask; alias fixup removed; for each accessor its
  bounds test removed, and its null test, raw SPU test or ref-poll test
  removed where it has one; the flag test of each out-of-line function
  removed; count removed for each of the 8 accessors; the count of one
  accessor under the next.
- loader (27): the parse of `PPU_FM_SPINDET` accepts `00`, is removed, or
  accepts a leading zero word; the detector is off by default; pages outside
  `ppu_vm_size` not flagged, or flagged only when the page starts outside;
  null-read pages below `0x1000` only; null-store page not flagged; alias
  pages not flagged; raw SPU register pages not flagged; ref-poll page not
  flagged; `SPU_ATOM_EA` does not send stores out, or `SPU_ATOM_EA=0` does;
  `PPU_FM=0` keeps the fast paths; a knob does not send accesses out; the
  flag word is not stored; the flag word starts clear; the counter is on for
  `PPU_FM_COUNT=0`, or does not send accesses out; a line comes too early; the
  period changed; a thread
  does not keep its slot; a thread line has no delta; the sum line has no
  sum; a thread line without calls; the sequence number does not increase;
  the first second does not start.

Run here on Windows with clang-cl, and on Linux with GCC 13 in a container.
clang on Linux and macOS, and the compilers of the CI hosts, are not run.
