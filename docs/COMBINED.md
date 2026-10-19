# Combined runtime testing

The `combined` branch in [robbe1912/ps3recomp](https://github.com/robbe1912/ps3recomp/tree/combined)
is the integration lane for shared fixes. Individual implementation commits and
their original branches remain available for later selective upstream proposals.
Do not submit this cumulative branch as one upstream PR. Upstream publication
requires the owner's approval.

## Included changes

Upstream baseline: `a2c20108af45174dbb7d3e2f87f6acaaef09c6b4` (v0.14.0),
merged at `8b244cf`. This replaces the earlier `5244db9` comparison point.

| Area | Implementation commits | Boundary |
| --- | --- | --- |
| Allocation alias protection / physical capacity | `8ad9fdb`, `dcd6f48` | Virtual alias gaps do not consume physical capacity; alias protection is a dependency. |
| Indexed strip/fan restart | `7db6730` | Handle restart before attribute fetch and preserve split-batch continuity. |
| Packed vertex attributes | `0b85442` | Decode CMP and packed components through the common fetch path. |
| Fragment source-2 absolute | `0d03c8b` | Apply DWORD3 absolute before negation; authored actual-decompiler regression `27559e1`. |
| CPU/device surface reads | `77cab17` | Acquire device-written pages through shared read synchronization. |
| AA surface layout | `12a158f` | Derive sample-grid extent from AA mode, independently of pitch padding. |
| SPURS taskset guest layout | `3ed7f07`, `34acdab` | Guest mask/field layout, including authored bitset checks. |
| Task dispatch ownership | `f7104c0`, `fda09a4` | Preserve explicit task identity through dispatch; adapter exercises the actual source. |
| Task exit/incarnations | `35239df`, `9bfe08d`, `8e40617` | Distinguish EXIT from entry return, reject stale ownership, release unlaunched claims. |
| Fragment HLSL capacity | `387d1fa` / upstream `75a3269` | Original atomic branch retained; also present in the upstream integration. |
| D3D12 window visibility | `d316fbc` / original `92885d4` | Show the host game window after creation. |

The inherited opt-in task tracing (`dbe0b4b`) is diagnostic, not a correctness
fix. Unfinished PVS/draw observers and the force-updated pointer-audit branch
are excluded. Merge presence alone is not evidence of a native game fix.

## Integration checks, 2026-10-14

On Windows x64, Clang 17.0.3, Ninja Release with
`PS3RECOMP_BUILD_TESTS=ON` and `BUILD_TESTING=ON`:

- Runtime build and all seven CTest tests pass: CPU-surface HLE, sync stress,
  three condition tests, fiber switching and codec stub.
- All 24 `tools/test_*.py` commands exit successfully. Interpreter ILH explicitly
  skips on Windows; successful exit is not coverage of its POSIX execution.
- Separate production-source fixtures pass indexed restart, packed vertex fetch,
  AA extent and signed homogeneous-W clipping with WARP pixel readback.
- The fork-native source-2 fixture passes; restoring the old missing-absolute
  decode makes it fail. It uses this tree's seven-argument shader API and calls
  constant collection separately. A prior cross-tree test incorrectly supplied
  the MotorStorm header's eight-argument API; that result is invalid.

Standalone fiber and codec test targets now compile `guest_read_sync.c` because
their inline guest reads reference its state/dispatcher. Linux/macOS direct SPU
compile lists include the task-lifecycle module alongside workload code.
All four CI workflows run on pushes to `combined`; hosted results remain the
authority for platforms not executed locally.

Initial hosted runs exposed the same read-coherence dependency in standalone
network, savedata and SPU fixtures. Their direct compile commands now link the
module. The guest-field runner also accepts `clang-cl` from PATH when the optional
local `MV_CLANG` override is absent. Keep CI failures visible while validating
these integration dependencies; none of these changes suppresses a test.

These are authored/runtime tests without original game inputs. No native
MotorStorm run was performed with this merged fork, no speedup was measured,
and no bike/menu/race acceptance follows. FFmpeg decoding was not enabled in
the local CTest configuration. Keep source/API admission, runtime semantics,
native visuals and gameplay acceptance separate when selecting changes.

## Policy argument and observer trial

Combined head `e113b2a0c70eca07fb243b22dda6aa560c0bf535` merges atomic branches
`codex/native-data-watch` (`1b84c93`) and `codex/spurs-policy-u64` (`680f097`).
The former is a generic Win64 owned-process hardware write observer with authored
write/new-thread/dump/detach/deadline regressions; see NATIVE_DATA_WATCH.md. The
latter preserves scalar-u64 high/low argument halves instead of duplicating the
low half. Its actual-store authored fixture fails three cases before and passes
five after. Local Release build/CTest passed 7/7. All four hosted workflows passed
at that exact head. These tests contain no game inputs.

The selectively ported native game trial removed the observed nonzero writes in
a bounded slot watch and restored contiguous descriptors, but still waited for
an unfinished PVS producer. Its first watched native run showed black instead
of the background; two subsequent unwatched runs on the same native executable
restored the desert, with artifacts and no bike/race. This is a run-dependent
observation, not a consistently reproduced ABI regression or proof of debugger
causality. A suspended read-only capture mapped the outstanding producer to a
unique idle MFC engine with all tags complete and no list stall, narrowing the
next investigation to guest progress. No full-game acceptance follows, and the
selective trial remains off the game's main branch.
The merged fork runtime as a whole has not been substituted into the native game.
Keep the atomic branches for later review; no upstream PR has been opened.


## Extended single-precision value correction

Atomic `eccae411` plus runner linkage fix `79ac64c` are integrated as
`057c56c`/`492fdfa`. The generic FI/FA/FS/FM/FMA/FMS/FNMS value helpers implement
IBM SPU ISA v1.2 extended finite exponents, FTZ, positive zero, truncation,
saturation and exact fused products. Authored exact-rational tests exercise
36,816 lanes through actual helpers and actual instruction dispatch, including
the zero-normalization NaN regression. Both modes and existing 63 helper tests
pass on the atomic branch; its Windows and Linux CI pass at `79ac64c`.

MotorStorm selectively adopted the two runtime headers in
[PR 117](https://github.com/robbe1912/motorstorm-mv-recomp/pull/117).
Its native observation retains original bike/rider draws and finite sampled
preview velocities/transforms, but still has material/shader defects and has
not demonstrated race entry or a minimum frame rate. FPSCR accumulation,
comparisons/conversions and hardware identity of estimate tables are outside
this value correction. The whole combined runtime is not overlaid onto the game.


## Scoped exact SIMD value acceleration

The atomic `codex/spu-float-simd` change is tracked by MotorStorm issue 142.
It accelerates ordinary-range FA/FS/FM/FMA/FMS/FNMS in both compiled SPU code
and interpreted dispatch. Exponent-255 inputs and overflow-boundary outputs
retain the exact integer implementation. FMA requires a checked CPU feature
and enabled OS AVX state; other architectures retain the scalar path.

Public drain/interpreter boundaries enable a thread-local capability scope.
Only individual optimized arithmetic operations change MXCSR to truncation,
FTZ/DAZ and masked exceptions, restoring the entire previous value afterward.
Other host services keep their previous rounding environment. Nested scopes
and outer restoration after an inner longjmp are covered. This does not alter
PM interpretation policy, instruction execution, FPSCR support or guest data.

The independent rational oracle passes 36,816 lanes each through actual
helpers and interpreter dispatch. The production regression compares 1.92
million lanes against the exact integer helpers under all four host rounding
modes, exercises mixed extended values, hardware admission, scalar fallback,
constant operands, nested restoration and an unmasked caller environment.
Existing 63 helper checks and the productive IRET regression also pass.

An authored Windows checked-O1 helper-throughput trial used five alternating
scalar/SIMD pairs, each 20 million iterations of four-lane FMA followed by add.
All paired checksums matched. Scalar median was 1.5942413 seconds (range
1.5630011-1.6152887); SIMD median was 0.4199008 (0.4126122-0.4269420).
This is a normal-range helper benchmark, not a native gameplay FPS result.
Native Windows/D3D12 performance and source-recompile acceptance remain open.


## Upstream master synchronization (2026-10-19)

MotorStorm issue #145 integrates upstream master `39a3629` (v0.15.0 release
integration and subsequent golden update) through atomic branch
`codex/upstream-master-20261019`. Fork master fast-forwards separately; combined
retains the existing modular corrections and both histories.

Conflict resolutions retain upstream linked MFC ownership and stopped-thread
range cleanup, with generation invalidation for the fork's TLS lookup cache.
The upstream extended/double/NEON helpers remain; checked x86 SIMD wraps only the
six matching single-precision arithmetic helpers. Lifted jobs publish task-exit
results and release their context engine. Shader translation retains upstream
shadow/gamma/depth export and color-input clamping, together with the fork's exact
PK2H lowering and source sign handling. Bit-level GPU fixtures now source raw
values from unbounded TC0 instead of correctly clamped color inputs.

Generated PPU inline and raw loads synchronize device-owned guest pages before
loading. The production-header regression failed seven checks without this
integration, then passed all nine checks with eight synchronization callbacks.
Plain unmarked memory retains the upstream inline path; diagnostic delegation
remains intact. Nine targeted Windows CTests pass, including WARP/hardware shader
bits, checked GPU ownership, exact SIMD, MFC cache/range cleanup and decompilation.
Standalone SPU oracle/IRET checks and combined platform CI are recorded on #145.

This dependency merge does not replace the curated MotorStorm vendor or prove
native race performance. Windows Vulkan foundation remains issue #144; retail
mstream remains parked. The cumulative branch is not an upstream proposal.

## Windows Vulkan foundation (MotorStorm #144)

Atomic branch: `codex/vulkan-windows-bootstrap`. Windows can opt into the
shared Vulkan draw engine, using the system loader, SDL2 and glslang/SPIRV.
The authored host is portable to Windows, including its actual guest-thread
stack check. Requested guest translation/setup fails explicitly when absent.
Windows CI builds this path with pinned header/SDL dependencies and checks
that missing translation produces no frames. D3D12 remains available.

Local Windows hardware checks exercised all fourteen existing guest-program
scenes on an NVIDIA GeForce RTX 3080, plus the guest-thread stack contract.
The separate missing-translator build passed its rejection CTest. Validation
and final platform CI results are recorded in MotorStorm #144. These authored
contracts do not establish title assets, normal PLAY, gameplay or FPS.
Downstream native binding and operation-level Vulkan serialization remain
follow-up work; no curated game vendor tree was replaced by this change.

## Native C++ nonlocal returns (MotorStorm #147)

Atomic branch: `codex/ppu-cpp-nonlocal`. The optional
`--cpp-nonlocal-jumps` lowering preserves standard setjmp/longjmp returns in
generated native C++ using context, return PC and saved stack pointer to select
the receiving frame. Standard symbol names and ABI thunks identify the calls;
no game addresses are encoded. C output remains the default; this option
requires a C++ source extension. The executed generated-code regression covers
recursive unwinding, ABI thunks and foreign-context propagation, and verifies
that the historical lowering fails. This prerequisite proves return behavior,
not title boot or performance.

## Required native Vulkan window (MotorStorm #146)

Atomic branch: `codex/vulkan-required-window`. Hosts may request
`PS3RECOMP_VK_REQUIRE_WINDOW=1`; failed SDL/WSI initialization then fails the
backend, and loss of the required window asks the host to stop. Optional
offscreen operation remains available. The public authored-host regression
executes a missing SDL driver negative control, plus all fourteen hardware
guest-shader scenes. Loader debug messages about skipped registry entries do
not count as skipped host tests; requested validation insertion is still
checked explicitly. This establishes the window contract, not title boot.

## Native profiling observers (MotorStorm #148)

Atomic branch: `codex/native-host-observers`. Restores the generic thread-name
and bounded load-stage event utilities used by the native host. Load tracing
is opt-in, serializes concurrent initialization/writes, escapes path fields,
and reports exhaustion/write failures. Thread log deduplication is synchronized;
Windows and POSIX thread naming use their respective platform APIs. The
executed authored fixture checks actual thread names and event contents,
disabled operation, concurrent initialization and the event budget. These
observers neither change guest execution nor establish title performance.

## Completed native Vulkan readback captures (MotorStorm #146)

Atomic branch: `codex/vulkan-native-capture`. Exposes a renderer-thread BMP
export of the last completed GPU readback, without extra GPU submissions.
Capture before a present fails explicitly. The public authored host exercises
that rejection and exports all fourteen scenes; the hardware/validation runner
checks complete BMP structure and exact center-pixel agreement with the actual
readback. This provides observation data for native host integration; authored
captures do not prove original assets or performance.

## Explicit Windows audio-session mute (MotorStorm #150)

Atomic branch: `codex/windows-session-mute`. `AUDIO_MUTE=1` now sets and
queries the actual WASAPI process-session mute before playback starts; an
explicit requested mute failure is fatal. Guest PCM, consumption, device
scheduling and recording remain intact. `check_audio_mute_host.py` requires
two real mute/init/shutdown cycles with no backend warnings. This host
preference establishes no guest audio accuracy or title performance result.

## Staged native module ownership

Atomic branch: `codex/spu-staged-native-registry`; native integration blocker
[152](https://github.com/robbe1912/motorstorm-mv-recomp/issues/152).

The native image registry records compiled load bases and distinct helper
entries. Staged content must match current local-store bytes before its native
image owns a range; an unsupported load base cannot select stale primary-image
code. Ownership is context-local and invalidates on DMA replacement and native
local-store writes. The full resolver, musttail dispatcher and interpreter
rejoin use one ownership-aware lookup. Valid cached branches avoid content
hashing, and ordinary code uses bitmap admission rather than scanning module
contents. Complete payload registrations also support fragmented DMA staging.

The portable authored regression executes production registry, shared lookup
and musttail code against competing images, separate contexts, replacements,
helper entries, bounds and unsupported relocation. Its unchanged pre-fix
missing-API control must fail. The existing full-runtime lifted-start fixture
also checks actual full-resolver execution and same-source replacement.
This proves neither original-title Vulkan behavior nor sustained game FPS.
