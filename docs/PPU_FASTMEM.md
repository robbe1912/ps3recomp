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

## Check

```
python tests/check_ppu_fastmem_diet.py
```

The script needs clang-cl on Windows, or clang, gcc or cc with their C++
drivers elsewhere. Set `MV_CLANG` or `CC` to select a compiler. It compiles
the production header and the production `ppu_fm_arm` text with authored
stubs. No game input. No performance claim.

Mutants that the script rejects:

- header (11): detector switch test removed or inverted; threshold changed;
  equal changed to not equal; armed test removed; bitmap test inverted; line
  shift changed; bit mask changed; byte index changed; read test of
  `ppu_fm_diag` removed; write test of `ppu_fm_diag` removed.
- loader (4): the parse of `PPU_FM_SPINDET` accepts `00`, is removed, or
  accepts a leading zero word; the detector is off by default.

Run here on Windows with clang-cl, and on Linux with GCC 13 in a container.
clang on Linux and macOS, and the compilers of the CI hosts, are not run.
