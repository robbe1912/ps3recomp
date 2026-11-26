/* thread_name.h -- name host threads for CPU-time attribution.
 *
 * The menu build runs ~20 host threads and burns 4+ cores of guest compute.
 * Per-thread CPU samples attribute fine, but only to anonymous tids -- which
 * subsystem owns which core stays a guess until each thread says its role.
 * SetThreadDescription (Win10+) lands the name in ETW, Process Explorer,
 * Task Manager and crash dumps; pthread_setname_np does the same job on
 * POSIX dev boxes.
 *
 * Declaration-only by design: the call sites live in C and C++ files that
 * include this header mid-file, next to the CreateThread call being named.
 * The implementation is in thread_name.c.
 */
#ifndef PS3EMU_THREAD_NAME_H
#define PS3EMU_THREAD_NAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Name `thread_handle` (a Win32 HANDLE on _WIN32, ignored on POSIX, where
 * the CALLING thread is named). No-op when the platform cannot name threads
 * or the API is missing (pre-Win10). */
void ps3_thread_setname(uintptr_t thread_handle, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_THREAD_NAME_H */
