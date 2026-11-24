# CPU deformation buffer

MotorStorm paints wheel ruts into one 4096 x 4096 B8 surface and reads
single bytes of it on the CPU. Each read is a GPU round trip: about 45 reads
a frame, and each one waits for the render queue. The CPU deformation buffer
replays the paint draws into host memory and serves the reads from there.

`libs/video/rsx_deform_surface.inc` holds the code. `rsx_d3d12_backend.c`
includes it after `rsx_cpu_surface.inc`. `libs/video/rsx_deform_surface.h` is
the interface of the game: `rsx_deform_mode`, `rsx_deform_sample` and
`rsx_deform_wheel_log`.

## Switches

Everything is off by default. The backend reads the switches once, at the
first record.

| Switch | Meaning |
|---|---|
| `RSX_DEFORM_TARGETS=<offset>[,...]` | RSX offsets of level 0 of the surfaces, at most 4. Nothing is on without this list. |
| `RSX_DEFORM_BUFFER=1` | Mode on. Keep level 0 and row 0 of level 1 of each listed surface in guest memory. The game reads these bytes. |
| `RSX_DEFORM_SHADOW=1` | With the buffer: shadow mode. Keep the GPU path, paint private copies and compare them with the GPU bytes. |
| `RSX_DEFORM_CAPTURE=1` | Write the first 64 records into the listed surfaces to `rsx_deform_draws.txt`. |
| `RSX_DEFORM_CHAIN=1` | With the capture: write the next 64 records to `rsx_deform_chain.txt`. |
| `RSX_DEFORM_OP=fp16\|fused\|f32` | The pixel operation of the painter. `fp16` is the default. |
| `RSX_DEFORM_LOG=<frames>` | `300`. Interval of the `[DEFORM]` lines. Number of compared frames of the shadow mode and of frames with `[DEFORM_WHEEL]` lines. |

## Mode on

The painter obeys the two programs of the captured paint draws and the state
words of the backend:

- Position is attribute 0, the brush coordinates are attribute 8 and the
  intensity is the first byte of attribute 3.
- A stamp is two triangles. The raster rule is the top left rule of the host.
- The pixel operation is `fp16(saturate(fp16(brush - 0.25) * colour.x + dest))`.
  `dest` is the byte of the target, `brush` is a byte of the B8 brush.
- The brush comes from the decoded texture: nearest or bilinear, with the
  address modes of the unit.
- A draw with another primitive type, without one of the three arrays or with
  a brush of another layout is not painted. The backend logs the reason once.

Level 0 is written into guest memory, so the read paths of
`rsx_cpu_surface.inc` serve every read of that range without a copy of the
GPU surface. Row 0 of level 1 is the mean of each 2 x 2 block of rows 0 and
1, rounded half up. The mip chain stays GPU work.

The GPU keeps the paint draws, because the shaders of the game sample the
surface.

## Shadow mode

Shadow mode proves the painter. It keeps the paint draws and the readback
path, so the game reads the GPU bytes. The painter also paints one private
copy per pixel operation. After each submitted batch the backend compares the
columns that the painter changed since the last compare with the GPU bytes.
A target compares at most once a frame and `RSX_DEFORM_LOG` frames in all.
When the mip pass ran, the compare also holds row 0 of level 1.

## Log lines

- `[DEFORM] target address=...` at the first paint of a target: the guest
  address, the size, the pitch and the mode.
- `[DEFORM] frame=...` each `RSX_DEFORM_LOG` frames that paint: draws, skipped
  draws, painted texels and dirty rows.
- `[DEFORM_SHADOW] frame=...` once per compared frame: `texels`, `equal`,
  `diff1` (a difference of one), `diffmax`, the first difference (`first`,
  `cpu`, `gpu`), the equal texels per operation (`op_fp16`, `op_fused`,
  `op_f32`) and the counts of row 0 of level 1 (`mip_*`, `neighbour`).
- `[DEFORM_SHADOW] done` after the last compared frame: the totals.
- `[DEFORM_WHEEL] frame=... x=... y=... level=... cpu=... gpu=...` for each
  read of the game from its native GetDepth: the four CPU bytes next to the
  four GPU bytes. `level` is `point` or `bilinear`.

## Check

Run `python tests/check_rsx_deform_surface.py`. It compiles the production
pixel operations, painter, mip row, sampler and shadow comparison next to a
model of the backend names and the production texture decode. The state is
the captured paint state. The check paints path stamps and a 2D stamp, reads
the four bytes of a read at the bottom row, checks the pixel operation against
an independent half rounding, the mip mean, the stamps that cover no pixel and
the row bounds. It rejects mutants of the production text.

## Open

No run has compared the CPU bytes with the GPU bytes. The shadow mode is the
proof of the pixel operation, the bilinear weights, the mip rounding and the
position steps.
