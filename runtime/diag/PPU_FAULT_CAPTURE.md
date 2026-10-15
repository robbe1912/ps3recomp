# PPU unresolved-call snapshots

Set `PPU_FAULT_CAPTURE_DIR` to an existing, empty private directory. The Windows
unresolved-call reporting path records one event per process. Disabled runs do
not create files. A requested capture on other platforms fails explicitly.

The event contains materialized `ppu_context` bytes and 256-byte regions at the
32 GPR values and CTR, interpreted as 32-bit guest addresses. A pure optional EA
normalizer lets an integration retain both raw and effective regions when they
differ. The flat-memory hook supplies no normalizer; integrations with aliases
must supply their own normalizer. It must not perform MMIO, coherence or guest
writes. There are at most 66 regions, each at most 256 bytes, plus the context
and JSON metadata. Out-of-range, unreadable and partial reads are explicit.

`fault.json` is written last and binds host PID/TID, creation FILETIME, UTC,
QPC, guest thread, target and LR. Each binary is created exclusively. Output
creation/write failures abort with an explicit diagnostic. Successful capture
continues the existing unresolved-call error path. It does not repair pointers
or change resolution, returns or error reporting.

Reads of different regions are sequential. Optimized context memory is not a
complete architectural trace. These bytes do not identify the first corrupting
writer, prove resource ownership, or establish gameplay correctness. Keep game
memory private. The owner launcher must bind its executable/ELF identities and
control process lifetime.

Run `python tools/test_ppu_fault_capture.py` with the Windows C++ toolchain on
PATH (or `MV_CLANG`). The authored fixture exercises the actual header across
16 threads and two translation units: disabled behavior, the one-event cap,
raw/normalized bytes, unreadable/overflow addresses, context/VM immutability,
and explicit output/collision errors. Production-path evidence remains a
separate native observation.
