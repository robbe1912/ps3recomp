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
  that does not call `ppu_hle_init()` keeps `ppu_fm_diag` at 0, the default.
- The out-of-line family of the fork (`vm_read*`, `vm_write*` in
  `runtime/ppu/ppu_loader.cpp`) has its own detectors and prints. It does not
  call the fire halves `ppu_fm_hotread*`. The game family calls them.
- `runtime/ppu/ppu_loader.cpp` defines `g_barrier_sync_watch` as `volatile`,
  as the game tree does. The header declares it `volatile`.

## Rules for callers

- Include the header, then rename the `vm_read*` and `vm_write*` names to the
  `ppu_fm_*` accessors. The rename is behind `#ifndef PPU_FASTMEM_NO_RENAME`.
  `runtime/ppu/ppu_loader.cpp` defines the out-of-line family, so it must
  define `PPU_FASTMEM_NO_RENAME`.
- Call `ppu_hle_init()` before any guest code runs. `ppu_fm_arm()` is the only
  writer of `ppu_fm_diag` and of `ppu_fm_spin_det`.
- `PPU_FM=0` in the environment routes every access through the out-of-line
  family. It is the kill switch.
- `PPU_FM_SPINDET=0` turns the hot-spin detector off. Only that exact value
  does.
- `ppu_fm_arm()` must run after the host set `ppu_vm_size`. The header reads
  `ppu_vm_size` on each access.
- `PPU_FM_COUNT` in the environment turns the call counter on. Only the exact
  value `0` keeps it off. The counter is a diagnostic: it sets `ppu_fm_diag`
  to 1, so each access takes the out-of-line family.

## Layout

| Accessor | Tests, in order | Then |
|---|---|---|
| `ppu_fm_read8`, `read16`, `read64` | alias window, `ppu_vm_size`, `ppu_fm_diag`, address below `0x10000` | read sync, load, byte swap, detector |
| `ppu_fm_read32` | alias window, `ppu_vm_size`, `ppu_fm_diag`, address below `0x10000`, raw SPU window, GCM ref-poll address | read sync, load, byte swap, detector |
| `ppu_fm_write8`, `write16`, `write64` | alias window, `ppu_vm_size`, `ppu_fm_diag` or `g_barrier_sync_watch`, address below `0x1000` | reserved-line bitmap, store |
| `ppu_fm_write32` | alias window, `ppu_vm_size`, `ppu_fm_diag` or `g_barrier_sync_watch`, address below `0x1000`, raw SPU window | reserved-line bitmap, store |

- A test that is true sends the access to the out-of-line family
  (`vm_readN`, `vm_writeN`). The access is then the access of a build without
  the paths.
- The detector counts the consecutive reads of one address through one
  accessor. At 200000 it calls `ppu_fm_hotreadN` one time and starts again.
- The store paths test the reserved-line bitmap inline. A reserved line goes
  to `vm_writeN`, which has the locked-line path.

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
- The default path of each accessor has no code of the counter.

## Check

```
python tests/check_ppu_fastmem_diet.py
```

The script needs clang-cl on Windows, or clang, gcc or cc with their C++
drivers elsewhere. Set `MV_CLANG` or `CC` to select a compiler. It compiles
the production header and the production `ppu_fm_arm` text with the counter
of the production `ppu_loader.cpp` with authored stubs and an authored clock.
No game input. No performance claim.

Mutants that the script rejects:

- header (26): detector switch test removed or inverted; threshold changed;
  equal changed to not equal; armed test removed; bitmap test inverted; line
  shift changed; bit mask changed; byte index changed; read test of
  `ppu_fm_diag` removed; write test of `ppu_fm_diag` removed; count removed
  for each of the 8 accessors; the count of one accessor under the next.
- loader (16): the parse of `PPU_FM_SPINDET` accepts `00`, is removed, or
  accepts a leading zero word; the detector is off by default; the counter is
  not armed, is on for `PPU_FM_COUNT=0`, or does not arm the diagnostic
  family; it counts with the switch off; a line comes too early; the period
  changed; a thread does not keep its slot; a thread line has no delta; the
  sum line has no sum; a thread line without calls; the sequence number does
  not increase; the first second does not start.

Run here on Windows with clang-cl, and on Linux with GCC 13 in a container.
clang on Linux and macOS, and the compilers of the CI hosts, are not run.
