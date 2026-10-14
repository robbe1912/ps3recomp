# Guest D3D12 sampler candidate

Unmerged and unaccepted in native game boot. Fixed font unit0 point/clamp and
other-units linear/wrap ignore guest material state. Snapshot guest sampler
registers per draw and bind immutable descriptor tables in parity banks, with
complete-state deduplication and explicit capacity errors. Preserve b0/SRV/b1
root slots, adding a sampler table at3. Final generated HLSL supplies a constant-
index use mask stored in the PSO cache. Do not validate unused inherited registers.

Decoder covers point/linear/mip filtering, repeat/mirror/clamp/border/mirror-once,
signed bias, LOD limits and anisotropy. Legacy clamp/mirror-border and convolution
are explicitly unsupported. Packed half aliases retain their existing supported
point/clamp or linear/wrap policies, now selected from guest state and keyed in
shader identity. This does not add other alias addressing/filter policies.

Run `python tests/check_rsx_sampler.py` in a Windows clang-cl/SDK developer
environment; optional `--device hardware` selects hardware with no WARP fallback.
Authored D3D12 textures, root signatures, two descriptor heaps, immutable sampler
tables, GPU draws and fenced float readbacks use no game inputs. Seven cases
cover ordinary modes and preserve point/clamp font controls; old fixed states
fail five, corrected WARP/hardware pass all. This tests descriptor decoding and
GPU table use, not the complete backend recorder/cache. Logs/hashes stay in temp.

Both runtime builds pass. Native named-US MotorStorm boot fails explicitly on
sampled u0 FP01F8C8C1, address60030303/filter04074000/control080078000, mask0001.
Enums identify Gaussian convolution; hardware coefficients are unproven. No
approximate linear mapping or guessed Gaussian implementation is introduced.
The candidate stays off combined until convolution is validated and native boot
and moving terrain are rechecked. Original assets, shaders and captures remain
private. No external PR requested; see MotorStorm issue97 and canonical graphics
research for exact EXE/ELF hashes, runs and limitations.
