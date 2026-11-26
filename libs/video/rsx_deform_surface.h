/* rsx_deform_surface.h -- the CPU deformation buffer of the D3D12 RSX backend
 * (#346).
 *
 * rsx_d3d12_backend.c defines these three functions in
 * libs/video/rsx_deform_surface.inc. The game calls them from its native
 * GetDepth (src/motorstorm/deformation_query.cpp of MotorStorm). Each
 * function does nothing when the buffer is off. */
#ifndef PS3RECOMP_RSX_DEFORM_SURFACE_H
#define PS3RECOMP_RSX_DEFORM_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0: the buffer is off. 1: on, the game reads the bytes of the CPU buffer.
 * 2: shadow mode, the game reads the GPU bytes and the CPU bytes are logged
 * next to them. The switches are RSX_DEFORM_TARGETS, RSX_DEFORM_BUFFER and
 * RSX_DEFORM_SHADOW. */
int rsx_deform_mode(void);

/* The four bytes GetDepth reads from the surface at guest address `address`:
 * byte k is at guest address address + offsets[k]. The function writes the
 * bytes that the CPU buffer holds (level 0 and row 0 of level 1) to `out`.
 * It returns how many it wrote: 0 to 4. The count is 0 when no record holds
 * the surface, the buffer is off, or the record changes for more than 50 ms.
 * The caller then reads the guest memory. A record that the renderer changes
 * now (a paint) ends first: the read waits for that end. */
int rsx_deform_sample(uint32_t address, const uint32_t offsets[4],
                      unsigned char out[4]);

/* Called by rsx_deform_sample() while it waits for the end of a paint: it
 * lets the renderer thread run. */
void rsx_deform_yield(void);

/* Shadow mode only; does nothing in the other modes. Writes one
 * [DEFORM_WHEEL] line for one read of the game: frame, x, y, the sample mode
 * `level` ("point" or "bilinear") and the four bytes of the CPU buffer `cpu`
 * next to the four bytes of the GPU `gpu`. `address` and `offset` are those
 * of the first byte; they give x and y. The lines stop after RSX_DEFORM_LOG
 * frames. */
void rsx_deform_wheel_log(uint32_t address, uint32_t offset, const char* level,
                          const unsigned char cpu[4], const unsigned char gpu[4]);

#ifdef __cplusplus
}
#endif

#endif
