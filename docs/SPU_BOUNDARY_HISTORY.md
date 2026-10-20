# SPU boundary history

`spu_step` has an optional diagnostic callback immediately after instruction
fetch, before decoding or execution. It never changes target registers, memory,
control flow or completion. With `SPU_BOUNDARY_DIR` unset, it records nothing.

For a private capture, create an empty output directory and set all of:

- `SPU_BOUNDARY_DIR`: existing directory; files are created exclusively.
- `SPU_BOUNDARY_PCS`: comma-separated aligned numeric LS instruction addresses.
- `SPU_BOUNDARY_CAPACITY`: 16 through 1024 records per context.
- `SPU_BOUNDARY_LS_START` and `SPU_BOUNDARY_LS_LENGTH`: a valid LS range of 1
  through 4096 bytes. The entire 2048-byte register file is always copied.
- Optionally `SPU_BOUNDARY_POLICY_ONLY=1` to restrict callbacks to policy mode.
- Optionally supply both `SPU_BOUNDARY_MATCH_LS` and `SPU_BOUNDARY_MATCH_BE32`
  to require a big-endian LS word value before allocating a context slot.

Select independently admitted boundaries, particularly memory stores and their
successors, rather than polling loops. The filter tests a bitmap. Only selected
boundaries acquire registry/context locks, copy snapshots and perform file I/O.
There are at most 32 retained context slots; capture errors, including overflow,
are visible on stderr. They invalidate completeness claims. Context addresses
can be reused and do not by themselves identify a new job or submission.

Each private `boundary-PID-SLOT.bin` has a 4096-byte JSON header and a circular
array. A slot is invalidated before replacement and published with matching
leading/trailing sequence numbers after its bytes have been written and the
library buffers flushed. No per-fetch file opening or fingerprinting occurs.
`python tools/read_spu_boundary.py PRIVATE_FILE` reports retained sequences,
overwritten prefix length, gaps and incomplete slots; it exits 2 for gaps or
incomplete slots. Retain the launch identity, exact runtime source/image hashes
and configuration alongside these files. Their register/LS payloads can contain
private original data and must stay outside version control.

These are fetched, pre-execution observations. The LS copy is not an atomic
snapshot against all other writers. A successor observation can help constrain
an earlier store, but a missing observation does not prove an instruction never
retired. Per-context sequences are observation order, not a cross-context global
order. A context can be observed from multiple native threads: keep the TID
when comparing a store with its immediate successor. Even a single-TID witness
does not establish object lifetime, job identity or the absence of other writers.
A later stopped LS read can match the page maps while differing elsewhere; do
not promote a partial match to a full-state identity. Instrumentation changes
timing and provides no performance or game acceptance proof.

`python tools/test_spu_boundary.py` checks the actual writer in C and C++, PC and
policy filtering, wrapping with explicit overwrite count, concurrent callbacks
and torn-record rejection. It also executes the actual interpreter's authored
CPU self-test: removing only the callback leaves architectural results passing
but loses the required trace; restoring it captures both sides of an LS store.
No game files are required. On Windows use `MV_CLANG` or a configured `clang-cl`.

Linux and Windows hosted workflows passed the implementation at exact commit
`c6195945c40d9d708293090cbf0bb62221ff3919` (runs 38558780467 and 38558784069).
Native diagnostic use exposed the importance of retaining overwrite counts,
actual writer TIDs and an exact PE/PDB archive before relinking. These are
capture-method findings, not a target-runtime correctness fix.

## Bounded full-context capture

The actual interpreter hook supplies `sizeof(*ctx)` to the writer. Setting
`SPU_BOUNDARY_CONTEXT_LIMIT` to 1 through 16 selects version-2 records: each
includes the raw host context and the full 256-KiB LS in addition to the legacy
GPR/window fields. Each context stops recording after that many observations;
execution continues normally. Missing/oversized context sizes are reported as
capture errors, rather than substituted with zero state. Without this option,
the existing version-1 ring format and wrapping behavior are preserved.

`SPU_BOUNDARY_FIRST_PC` optionally requires the first observation for each
context to match one of the selected PCs. `SPU_BOUNDARY_ARM_FILE` optionally
gates observation until that file exists. Only selected boundaries poll for it,
at most ten times per second per host thread; after arming, no further file
checks occur on that thread. These controls select evidence and do not change
instruction results, service responses, target state, or completion.

Full-context records require the exact context header/source and ABI layout
from their build. Raw pointers are opaque evidence, never valid replay pointers.
Retain matching EXE/PDB/build identities and explicitly reconstruct any required
pointer ownership. Other writers can change shared fields during copying, so
the record is not an all-writer atomic snapshot. Same-address reuse, missing
external memory and asynchronous service state still constrain replay. A
pre/successor pair does not establish arbitrary service semantics or full PM
parity. Recording adds substantial latency and is unsuitable for FPS claims.

There are at most 32 context slots; each version-2 record has at most 512 KiB of
raw context plus 256 KiB LS and legacy metadata. The 16-record limit bounds
payload volume below 400 MiB. Files remain private. The reader validates record
layout and commit markers; it never synthesizes missing context or tail bytes.

`python tools/test_spu_boundary_context.py` first rejects the legacy writer for
missing full context, then checks C/C++ nonzero event/decrementer fields, the
last LS byte, first-PC gating, bounded recording, arming, invalid configuration,
missing/oversized context sizes and torn records. It also executes the actual
interpreter's authored CPU program, preserving nonzero context fields and both
sides of its LS store. No game bytes are used. These are capture-method checks,
not a compiled-execution or performance verdict.
