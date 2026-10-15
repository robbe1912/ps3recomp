# SPU single-precision value regressions

`tools/test_spu_float.py` compiles the actual helper header and compares all
four lanes with a separately authored Python `Fraction` model. Add
`--interpreter` to execute authored instructions through `spu_interp.c` instead
of calling helpers directly. `--header-root` selects another runtime checkout
for adoption or unchanged-source red checks. Windows uses `MV_CLANG`, `CC` or
`clang-cl`; POSIX uses `CC` or `cc`.

The source is IBM's [SPU ISA v1.2](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf),
section 9.1 and the FA, FS, FM, FMA, FMS, FNMS and FI instruction definitions.
The reference uses exact fractions followed by a single final truncation,
independently of the runtime's integer alignment and sticky-bit implementation.
Cases include exponent-255 finite values, signed zero, denormal inputs,
saturation, cancellation, positive-zero underflow, unbounded fused products,
all FI exponent fields and packed base/step endpoints, deterministic random
vectors, and zero normalization through actual FRSQEST/FI/FM helpers or opcodes.
Each invocation retains compiler output and a JSON result in `temp/checks`.

The implementation represents decoded significands as integers. Multiplication
has at most 48 significant bits. Addition aligns leading bits at bit 60, with
sticky right shifts; close operands retain all bits necessary for cancellation,
and distant operands retain the direction of truncation. Fused operations do
not flush or saturate the intermediate product. FI evaluates the specified
packed base/step equation without treating its exponent as an IEEE exponent.

This suite verifies the modeled value semantics, not hardware-bit identity for
implementation-dependent approximations, FPSCR status flags, unchanged estimate
tables, comparisons/conversions, or any game's rendering/performance. No guest
instructions or game data are included in these authored programs.
