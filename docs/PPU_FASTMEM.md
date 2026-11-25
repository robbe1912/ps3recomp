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
- `PPU_FM3=0` before the include selects the layout before step 3: the
  read accessors out of line, and the alias fixup in the out-of-line
  function only. The default `1` is the layout of the section "Step 3".

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
- With `PPU_FM3 0` and `PPU_FM_INLINE_FAST 1`, the fire helpers get the
  address of a label at the call site as caller address. With `PPU_FM3 1`,
  the fire half is `ppu_fm_fireN`, a function that is not inline. It reads
  its own return address: the call site in the lifted function.

## Step 3: `PPU_FM3 1`

The default. Two changes to the layout above.

- The read accessors hold no label address. Clang 17 does not inline a
  function that holds a label address and a live thread-local access
  (remark: `blockaddress used outside of callbr`). With `PPU_FM3 0`, each
  lifted read is a call of one out-of-line copy of the accessor. With
  `PPU_FM3 1`, each read is inline at the call site, as each write is.
- The gate gives the alias answer of a page of the window inline
  (`ppu_fm_window`). The access stays inline for an IDENTITY page (the EA
  stays) and for an ALIAS page (the body runs on `EA - 0x05000000`). The
  gate makes one load of the page state for that, only on a flagged page of
  the window. A MIXED page, each set flag bit and an access outside
  `ppu_vm_size` go to the out-of-line function, which has the fixup and
  the reports. The bounds test stays on the inline path. So does the
  test of the reserved lines of the store body, on the answered EA.
- `read32` at `ppu_hle_inject_base + 0x2008` goes to the out-of-line
  function for the GCM ref-poll fence.
- The page state comes from `sysmem_spu_alias_page` of
  `runtime/syscalls/spu_ea_alias.c`: one atomic byte per 4 KB page of the
  window, with the values `PPU_FM_ALIAS_IDENTITY`, `PPU_FM_ALIAS_ALIAS`
  and `PPU_FM_ALIAS_MIXED`. The fixup writes each byte before it publishes
  a record. `PPU_FM_ALIAS_PAGES` and `SPU_ALIAS_PAGES` must stay equal.
  The header has two proofs for that path: the window and its primary
  image hold no null address and no raw SPU register.
- An EA with bit 31 set is a plain page for the gate, as before.

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

## Check of step 3

```
python tests/check_ppu_fastmem3.py
```

The script checks the layout of `PPU_FM3 1` against `PPU_FM3 0`. It compiles
the production header, the production `ppu_fm_arm` text and the production
`runtime/syscalls/spu_ea_alias.c`, with authored guest memory (a reserve of
4 GiB) and authored stubs of the out-of-line family. Each of the three
builds (default, `PPU_FM3=1`, `PPU_FM3=0`) runs in both layouts and in ten
modes: no arm, each access class, each diagnostic bit, bounds, the hot-spin
detector on and off, and the reserved-line store. The access classes are a
plain page, an EA with bit 31, an IDENTITY page, an ALIAS page, two MIXED
pages, the null page, a raw SPU register, the GCM ref-poll address and
pages outside `ppu_vm_size`. Each class runs for reads and writes of each
width. The script lists the symbols of the object: the default build in
layout 1 has no out-of-line accessor, and `PPU_FM3=0` in layout 1 has the
four read accessors.

Mutants that the script rejects:

- header (24): ALIAS page answered as IDENTITY; IDENTITY page answered as
  ALIAS; alias delta changed; MIXED page answered inline; a flag bit does
  not send a window page out of line; a page outside the window answered
  inline; bounds test of the alias answer removed; window lower bound at
  zero; window over the whole address space; upper bound past the raw SPU
  page (the proof fails at compile time); ref-poll fence of `read32`
  skipped; the gate without the page table; the gate without the flag
  word; the store body on the address before the answer; the fire function
  with a null caller address; the fire dispatch without the caller address
  test; `PPU_FM3` defaults to 0; null caller address in layout 0; page
  state always from the first page; a read accessor with a label address
  again; byte order of `read32` turned; byte order of `write16` turned;
  null test of `read32` removed; raw SPU test of `read32` removed.
- alias file (4): ALIAS state published as MIXED; page table not updated;
  EA bit 31 rule lost; a partly covered page published as IDENTITY.
- arm text (1): alias pages not flagged.

Run here on Windows with clang-cl. The POSIX branch of the guest memory
(`mmap`) is not run.

## Step 4: `PPU_FM4 1`, move 1

The default. `PPU_FM3 0` forces it off. The lean gate moves the exact
tests of the gate of step 3 into one byte per 4 KB page (`ppu_fm_kind`)
and into the out-of-line function.

- The call site makes one test. It loads the page byte of the EA with one
  acquire load (`ppu_fm_page`). Then it computes `(byte | ppu_fm_slow) &
  (kind | mask)` (`PPU_FM_LEAN`). A nonzero result sends the access to the
  out-of-line function. Two `typedef` proofs state that the AND keeps only
  the route bit of the kind and only the flag bits of the kind.
- The route bits are `PPU_FM_K_READ` (`0x08`) and `PPU_FM_K_WRITE`
  (`0x10`). `PPU_FM_K_SYNC` (`0x20`) names the read sync as the writer.
  No route bit shares a position with a flag bit of the other kind, or
  with an owner bit.
- The bounds test is a decision of the page byte. `ppu_fm_arm` sets the
  route bits on each page that is not entirely inside `ppu_vm_size`, and
  on the page before the end: an access of up to 8 bytes that starts in
  its last 7 bytes ends past `ppu_vm_size`. The out-of-line function makes
  the exact test with the width.
- The GCM ref-poll address of `read32` goes to the out-of-line function,
  as in step 3.
- The read sync is a decision of the page byte. `vm_read_sync_mark_range`
  calls a new hook after its marks and after its flag. The hook is
  `vm_read_sync_mark_hook` (`include/ps3emu/guest_read_sync.h`). `ppu_fm_arm`
  installs `ppu_fm_mark_sync` (`runtime/ppu/ppu_loader.cpp`). The hook
  sets `K_READ | K_SYNC` on the pages of the range and `K_READ` on the
  page before the range. The out-of-line function of a read calls
  `vm_cpu_read_sync`, then the lean body of the call site. So the rule of
  the hot-spin detector is the same at each place.
- The writers of the page byte at run time use `ppu_fm_page_or`: one
  atomic OR with release order, after the state of the writer.
- The lean body of a read at the call site has the load and the hot-spin
  detector only. The store body at the call site is the body of step 3:
  the test of the reserved lines is still inline.

## Check of step 4

```
python tests/check_ppu_fastmem3.py
```

The script of the check of step 3 builds three paths in both layouts: the
lean gate (default), step 3 (`PPU_FM4=0`) and the path before step 3
(`PPU_FM3=0`). The same source must pass in each build. The lean path has
two more modes. `leanbits` checks each bit of the page byte after the
arm. `leansync` checks the read sync on the lean path, with its hook and
the order of the hook. `--only` takes a list of texts. The script then
runs the mutants with a matching name only. `--skip-actual` runs no build
without a mutant.

Mutants of step 4 that the script rejects:

- header (4): the lean gate without the page byte; the lean gate without
  the flag word; the read gate with the mask of a write; a writer that
  sets no bit.
- arm text (6): the page before the end not flagged; the hook not
  installed; the page before the range not flagged; only the first page
  of the range flagged; the hook flags stores too; the owner bit not set.
- read sync file (2): the hook is not called; the hook runs before the
  marks and the flag.
