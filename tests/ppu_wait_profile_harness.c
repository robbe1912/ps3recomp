/* Harness for check_ppu_wait_profile.py: the production header against an
 * authored context and a known wait. The parent process sets the switch. */
#include <stdio.h>
#include <string.h>
#include "ppu_wait_profile.h"

typedef struct {
    unsigned long long thread_id;
    unsigned long long gpr[32];
    unsigned lr;
} ppu_context;

#define LV2_ARG_U32(ctx, i) ((unsigned)((ctx)->gpr[3 + (i)]))

#ifdef _WIN32
#include <windows.h>
static void wait_milliseconds(unsigned ms) { Sleep(ms); }
#else
static void wait_milliseconds(unsigned ms)
{
    struct timespec t;
    t.tv_sec = (time_t)(ms / 1000);
    t.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&t, NULL);
}
#endif

int main(int argc, char** argv)
{
    ppu_context ctx;
    const char* mode = argc > 1 ? argv[1] : "on";
    int records = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 7;
    ctx.lr = 0x00ABCDEFu;
    ctx.gpr[3] = 3;

    /* An end without a begin writes nothing. */
    ppu_wait_profile_end();

    if (!strcmp(mode, "sleep")) {
        /* A real wait of 40 ms inside one bracket. */
        PPU_WAIT_PROFILE_BEGIN(&ctx, "cond_wait", LV2_ARG_U32(&ctx, 0));
        wait_milliseconds(40);
        ppu_wait_profile_end();
        records = 1;
    } else if (!strcmp(mode, "nested")) {
        /* A second begin on the same thread replaces the first. */
        ctx.gpr[3] = 5;
        PPU_WAIT_PROFILE_BEGIN(&ctx, "semaphore_wait", LV2_ARG_U32(&ctx, 0));
        ctx.gpr[3] = 9;
        PPU_WAIT_PROFILE_BEGIN(&ctx, "mutex_lock", LV2_ARG_U32(&ctx, 0));
        ppu_wait_profile_end();
        /* The bracket is closed: a second end writes nothing. */
        ppu_wait_profile_end();
        records = 1;
    } else {
        /* "on" and "off": one bracket that returns at once. */
        PPU_WAIT_PROFILE_BEGIN(&ctx, "cond_wait", LV2_ARG_U32(&ctx, 0));
        ppu_wait_profile_end();
        records = 1;
    }
    /* After a closed bracket, an end writes nothing. */
    ppu_wait_profile_end();
    printf("brackets=%d\n", records);
    return 0;
}
