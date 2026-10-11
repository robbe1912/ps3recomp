# Pointer snapshots at operation boundaries

`tools/pointer_stage_audit.py` checks every field in a declared pointer plan
against private native/reference snapshots. It normalizes base-relative
pointers across allocations, records matching and missing fields, and rejects
wrong sizes or changed immutable metadata. It never writes guest memory.

Start with one failing resource, its original input, and a working reference.
Record executable/input/configuration identities, PID/create time and the
capture's guest base. Validate the base with original executable bytes rather
than a preferred emulator address. Read-only captures at different times are
not necessarily stable: compare the resource before/after associated reads,
or pause and explicitly resume an owned reference. Save states before attaching
a debugger; an attached debugger can interfere with emulator save/shutdown.

Validate original instruction bytes and descriptor/callee mappings before
labeling a stage. Record an executed hook separately from a sampled native PC,
and distinguish PC from LR. Discover RSP register layouts where available; do
not assume every register after the GPRs has the same width. Confirm the actual
stopped thread. A connection or unclassified stop is not a captured operation.
Keep input navigation manual when requested, and retain tooling failures.

For Havok v4 big-endian 32-bit packfiles:

```sh
python tools/pointer_stage_audit.py havok-v4-plan private/raw.bin --output private/plan.json
python tools/pointer_stage_audit.py audit --plan private/plan.json --manifest private/stages.json --output private/report.json
```

The builder parses every local/global relocation into the plan, rejects
malformed tables and fingerprints the header plus fixup/trailer regions.
Finishing records are retained as an inventory. Finishing callbacks, vtables,
registry entries and referenced functions need separate runtime evidence; the
tool does not invent their expected values. Other packfile versions/layouts
are refused.

Output paths that alias an input, including hard links, are rejected.

The audit itself is format independent. A minimal plan looks like:

```json
{
  "schema": 1,
  "pointer_bytes": 4,
  "endian": "big",
  "size": 64,
  "fields": [
    {"offset": 0, "target_offset": 16, "label": "root"},
    {"offset": 4, "expected": 65536, "label": "validated static descriptor"}
  ]
}
```

Use exactly one of `target_offset` (relative to that capture's base) or
`expected` (an independently validated absolute address). Four/eight-byte
pointers and either byte order are supported. Field overlaps, out-of-range
sources/targets and pointer overflow are rejected. `invariants`, if supplied,
contains `{offset, size, sha256}` ranges that must match every snapshot. A
`raw_sha256` fingerprint must match captures declared as raw.

The manifest's file paths are relative to the manifest, or absolute:

```json
{
  "schema": 1,
  "ordered": true,
  "snapshots": [
    {"label": "input", "phase": "raw", "base": "0x20000", "path": "raw.bin"},
    {"label": "after relocation", "phase": "relocated", "base": "0x20000", "path": "relocated.bin",
     "identity": {"exe_sha256": "record the actual executable identity"}},
    {"label": "after finishing", "phase": "relocated", "base": "0x20000", "path": "finished.bin"}
  ]
}
```

Set `ordered` only when the captures are a validated sequence for the same
operation/object/thread. Use false for independent native/reference runs.
Raw zero pointers are expected to be unrelocated and cannot alone pass an
audit. A relocated stage must be supplied. Exit codes are 0 for matching
relocated snapshots, 1 for pointer differences, and 2 for rejected inputs.
All planned fields appear in every report, including matches. Identity fields
are retained as supplied evidence, not authenticated by this offline tool.

`earliest_compared_mismatch` names the earliest failing snapshot in a declared
sequence. It does not identify a first write or instruction. If a pointer is
already wrong immediately after relocation, observe the original store and
memory access implementation before changing finishing logic. If it becomes
wrong later, narrow the next capture interval. Do not disable a fault/guard or
substitute a reference buffer to make the comparison pass.

Keep game inputs, snapshots, states, plans and reports private and ignored.
Publish authored tools, synthetic tests, identities and scoped conclusions.
Run the synthetic regression with `python tools/test_pointer_stage_audit.py`.
