/* ps3recomp -- query the registered lifted PPU dispatcher entry.
 *
 * The registry is populated at boot before guest execution begins.  A caller
 * may obtain the current entry, then replace it through ppu_register_function,
 * to implement a shadow observer or a native replacement which can fall back
 * to the exact lifted function.  Lookup and registration must be quiescent
 * with respect to guest dispatch: the registry deliberately has no runtime
 * mutation synchronization.
 */
#ifndef PS3EMU_PPU_FUNCTION_REGISTRY_H
#define PS3EMU_PPU_FUNCTION_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;
typedef void (*ps3emu_ppu_function)(struct ppu_context*);

/* Returns 1 and writes the exact current entry when guest_addr is registered.
 * Returns 0 for a missing address or null output and leaves *out untouched.
 */
int ppu_get_registered_function(uint32_t guest_addr, ps3emu_ppu_function* out);

#ifdef __cplusplus
}
#endif

#endif
