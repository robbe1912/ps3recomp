# Held SPU float controls

The x86 float scope holds truncation, FTZ, DAZ and masked traps in MXCSR.
SIMD arithmetic uses those controls without a write for each operation.
The outermost entry records the caller controls.
Nested entry must not replace those controls, even if the current MXCSR changes.
A caller can already use SPU controls before entry.
Entry must record that caller too.

The double arithmetic and comparison helpers restore the caller controls for
host arithmetic. `spu_frds` does the same for its host conversion.
They return to the held controls before they return.
The fork's integer single conversions, comparisons and `spu_fesd` stay unchanged.
They do not depend on host rounding.
This port keeps their existing extended-range and denormal behavior.

`spu_run_with_halt` records a float mark before `setjmp`.
Both halt and stack-reset landings restore that mark.
The reset continuation enters a replacement scope and leaves it on return.
This restores both the hardware state and the thread-local scope state.

Run `python tools/test_spu_float_scope.py` with a configured C compiler.
On Windows, configure the Visual C++ include and library environment first.
Set `MV_CLANG` or `CC` to select the compiler.
The check compiles actual helpers for all four caller rounding modes.
It covers double arithmetic, comparisons, conversions, held SIMD arithmetic,
a caller that already uses SPU controls, and nested entry after controls change.
Three compiled mutants remove the host guard, switch outside a scope, or
replace the outer caller during nested entry.
The game carry also checks actual halt/reset landings with five mutants.
No check uses game input.

The tests cover value semantics. They do not cover SPU FPSCR status flags.
Measured result: none. No run yet.
