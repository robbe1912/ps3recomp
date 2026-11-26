/* thread_name.c -- see thread_name.h. */
#include "thread_name.h"

#include <stdlib.h>   /* getenv */
#include <string.h>   /* strncmp */

#ifdef _WIN32
#include <stdio.h>
#include <windows.h>

void ps3_thread_setname(uintptr_t thread_handle, const char* name)
{
    typedef long (WINAPI *setdesc_t)(HANDLE, const wchar_t*);
    wchar_t w[64];
    size_t i;
    setdesc_t setdesc;

    if (!thread_handle || !name) return;

    /* tid->role table into the .err log: SetThreadDescription does not
     * surface through Win32_Thread.Description on every build, so the
     * ground truth the CPU sampler joins against is this one line per
     * thread, printed by the creator right at the handle it just made.
     * GetThreadId (not GetCurrentThreadId): the CREATOR prints the NEW
     * thread's id. PS3_THRLOG=0 silences. */
    { static int s_log = -1;
      if (s_log < 0) { const char* e = getenv("PS3_THRLOG"); s_log = (e && *e == '0') ? 0 : 1; }
      if (s_log) {
        /* First print per tid only: PM lanes are re-created per dispatch and
         * high-churn titles stamp thousands of [thr] lines per minute
         * (bg_230: 206k lines/185s), all of it under the stderr lock while
         * the CPU sampler is trying to run. A repeated tid with a CHANGED
         * name still prints (rename visibility); same tid+name is silent.
         * Best-effort open-address set, no lock: a race only duplicates a
         * line, never loses the first one. */
        #define THR_SEEN_N 4096
        static unsigned long s_seen_t[THR_SEEN_N];
        static char         s_seen_n[THR_SEEN_N][24];
        unsigned long tid = GetThreadId((HANDLE)thread_handle);
        unsigned h = (unsigned)(tid * 2654435761u) & (THR_SEEN_N - 1);
        int fresh = 1;
        for (unsigned p = 0; p < 8; p++) {
            unsigned k = (h + p) & (THR_SEEN_N - 1);
            if (s_seen_t[k] == 0) {
                s_seen_t[k] = tid;
                snprintf(s_seen_n[k], sizeof s_seen_n[0], "%s", name);
                break;
            }
            if (s_seen_t[k] == tid) {
                if (strncmp(s_seen_n[k], name, sizeof s_seen_n[0]) == 0) fresh = 0;
                else snprintf(s_seen_n[k], sizeof s_seen_n[0], "%s", name);
                break;
            }
        }
        if (fresh)
            fprintf(stderr, "[thr] tid=%lu name=%s\n", tid, name);
      }
    }

    /* Resolved dynamically: keeps the binary loadable and linkable on
     * systems / SDKs without SetThreadDescription (pre-Win10). */
    setdesc = (setdesc_t)(void(*)(void))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (!setdesc) return;
    for (i = 0; i + 1 < sizeof(w) / sizeof(w[0]) && name[i]; i++) w[i] = (wchar_t)name[i];
    w[i] = 0;
    setdesc((HANDLE)thread_handle, w);
}

#else
#include <pthread.h>

void ps3_thread_setname(uintptr_t thread_handle, const char* name)
{
    (void)thread_handle;
    if (name) pthread_setname_np(pthread_self(), name);
}
#endif
