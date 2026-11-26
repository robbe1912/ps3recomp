/* Read-once environment switches for hot renderer/FIFO paths.
 *
 * getenv walks the whole environment block on every call (strchr/strncmp per
 * entry). Diagnostic switches read on every draw, bind, upload or FIFO pass
 * made that walk a measured share of the renderer thread. rsx_env_once(name)
 * returns the value the variable had at its first lookup, for the rest of the
 * process; the switches it serves are configured before launch.
 *
 * Entries are keyed by the address of the name literal (identical literals at
 * different call sites may get separate entries; both read the same value).
 * The value is copied, so a later environment change cannot leave a dangling
 * pointer. Lookup is lock-free after the first call per name; concurrent
 * first lookups agree because they read the same unchanged environment. A
 * full table falls back to a plain getenv, never to a wrong answer. */
#ifndef RSX_ENV_ONCE_H
#define RSX_ENV_ONCE_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RSX_ENV_ONCE_SLOTS 256u

typedef struct {
    _Atomic(const char*) key;   /* name literal address; NULL = free */
    _Atomic int state;          /* 0 free, 1 being filled, 2 ready */
    const char* value;          /* private copy, or NULL when unset */
} rsx_env_once_slot;

static rsx_env_once_slot s_rsx_env_once[RSX_ENV_ONCE_SLOTS];

static const char* rsx_env_once(const char* name)
{
    uintptr_t h = ((uintptr_t)name >> 3) * (uintptr_t)0x9E3779B97F4A7C15ull;
    unsigned at = (unsigned)(h >> 24) & (RSX_ENV_ONCE_SLOTS - 1u);
    for (unsigned probe = 0; probe < RSX_ENV_ONCE_SLOTS; ++probe,
         at = (at + 1u) & (RSX_ENV_ONCE_SLOTS - 1u)) {
        rsx_env_once_slot* s = &s_rsx_env_once[at];
        const char* key = atomic_load_explicit(&s->key, memory_order_acquire);
        if (!key) {
            const char* expected = NULL;
            if (!atomic_compare_exchange_strong_explicit(&s->key, &expected, name,
                    memory_order_acq_rel, memory_order_acquire)) {
                key = expected;              /* lost the race: inspect the winner */
            } else {
                const char* v = getenv(name);
                char* copy = NULL;
                if (v) {
                    size_t n = strlen(v) + 1;
                    copy = (char*)malloc(n);
                    if (!copy) return v;     /* slot stays filling; callers use getenv */
                    memcpy(copy, v, n);
                }
                s->value = copy;
                atomic_store_explicit(&s->state, 2, memory_order_release);
                return copy;
            }
        }
        if (key != name) continue;
        if (atomic_load_explicit(&s->state, memory_order_acquire) == 2) return s->value;
        return getenv(name);                 /* first lookup still in progress */
    }
    return getenv(name);
}
#endif
