# Optional draw batch cap

`RSX_SUBMIT_DRAWS=N` sets a draw batch cap. The default is off.
Use a decimal value from 16 through `MAX_DRAWS` (2048 in this branch).
An invalid value reports once and leaves the cap off.
The renderer reads the value once.

At a draw boundary, the initialized backend submits when the queued draw
count reaches or exceeds the cap. It uses the existing nonpresenting
capacity drain. The CPU-surface flush runs first. Capacity admission runs
last. Draws that merge without a boundary retain their current behavior.

Run `python tools/check_submit_draws.py` without game inputs. On Windows,
use a compiler environment with the Windows SDK and `clang-cl` on PATH,
or set `CLANG_CL` to its path. The check compiles the production parser
and boundary. It covers invalid values, cached selection, exact and
exceeded boundaries, initialization, and five compiled mutants.
The game carry also checks the actual submission and retirement bodies.

Measured result: none. No run yet.
