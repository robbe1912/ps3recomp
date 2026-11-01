#ifndef PS3EMU_SPU_HOST_LINE_COMMIT_H
#define PS3EMU_SPU_HOST_LINE_COMMIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Optional host waiter, independent of SPURS event flags and diagnostics.
 * Install or clear only while all SPU producers are quiescent; the callback
 * must remain valid until they have stopped. Runs after the coherence lock is
 * released, with the committed 128-byte aligned guest line. It is a wakeup
 * hint: waiters must always recheck their guest predicate, including after a
 * timeout because other writers do not necessarily trigger this callback. */
extern void (*g_spu_host_line_commit_hook)(uint32_t line);
#ifdef __cplusplus
}
#endif
#endif
