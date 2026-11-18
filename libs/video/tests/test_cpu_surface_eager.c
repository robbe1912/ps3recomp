/* Authored renderer model; the production eager-submission block is injected. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_commands.h"
typedef long LONG;
#define E_INVALIDARG (-2)
static LONG current_thread = 2;
static volatile LONG s_cpu_surface_thread = 2;
static int recording;
static int s_cpu_surface_profile;
static LONG GetCurrentThreadId(void) { return current_thread; }
static LONG InterlockedCompareExchange(volatile LONG* p, LONG value, LONG expected)
{
    LONG old = *p;
    if (old == expected) *p = value;
    return old;
}
static jmp_buf expected_failure;
static int failure_expected;
static void cpu_surface_fail(const char* operation, int result)
{
    if (failure_expected) longjmp(expected_failure, 1);
    fprintf(stderr, "FAIL: %s %d\n", operation, result);
    exit(2);
}
/* Renderer-owned state: reading it from another thread is a defect. */
static void require_owner(const char* what)
{
    if (GetCurrentThreadId() != InterlockedCompareExchange(&s_cpu_surface_thread, 0, 0)) {
        fprintf(stderr, "FAIL: %s read by another thread\n", what);
        exit(1);
    }
}
static int recording_for_owner(void) { require_owner("recording state"); return recording; }
#define s_cpu_surface_recording recording_for_owner()
typedef struct { const rsx_state* current_rsx_state; } D3DModel;
static D3DModel s_d3d_store;
static unsigned state_reads;
static void* d3d_for_owner(void) { require_owner("renderer state"); state_reads++; return &s_d3d_store; }
#define s_d3d (*(D3DModel*)d3d_for_owner())
/* PRODUCTION_HEX */
/* PRODUCTION_EAGER */
#undef s_d3d
#undef s_cpu_surface_recording

#define REQUIRE(x, message) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } } while (0)
static int parse_fails(const char* list)
{
    failure_expected = 1;
    if (setjmp(expected_failure)) { failure_expected = 0; return 1; }
    cpu_surface_eager_configure(list);
    failure_expected = 0;
    return 0;
}
static rsx_state next;
static void target(unsigned slot, u32 offset, u32 dma)
{
    next.surface_color_offset[slot] = offset;
    next.surface_color_dma[slot] = dma;
}
static void clear_targets(void) { memset(&next, 0, sizeof next); next.color_target = 1; }
static void reset(const char* list)
{
    current_thread = 2; s_cpu_surface_thread = 2; recording = 0;
    cpu_surface_eager_configure(list);
    clear_targets();
    s_d3d_store.current_rsx_state = &next;
}
int main(void)
{
    const u32 FB = CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER, HB = CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER;
    /* 1. The list: bounded hex offsets, no duplicates; empty means off. */
    cpu_surface_eager_configure(NULL); REQUIRE(s_cpu_surface_eager_count == 0, "unset list is off");
    cpu_surface_eager_configure(""); REQUIRE(s_cpu_surface_eager_count == 0, "empty list is off");
    cpu_surface_eager_configure("2a30480"); REQUIRE(s_cpu_surface_eager_count == 1 && s_cpu_surface_eager[0] == 0x02A30480u, "lower-case offset");
    cpu_surface_eager_configure("1,2,FFFFFFFF,0"); REQUIRE(s_cpu_surface_eager_count == 4 && s_cpu_surface_eager[2] == 0xFFFFFFFFu && s_cpu_surface_eager[3] == 0, "four offsets");
    const char* bad[] = {",", "1,", "1,,2", ",1", "123456789", "1,2,3,4,5", "1,1", "G", "1;2", "1 ", "-1"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (!parse_fails(bad[i])) { fprintf(stderr, "FAIL: accepted malformed list '%s'\n", bad[i]); return 1; }
    }
    /* 2. Only bound writes to listed surfaces mark the batch. */
    reset("02A30480");
    cpu_surface_eager_note(0x01A80500, 0x42380500); REQUIRE(!s_cpu_surface_eager_dirty, "unlisted surface does not mark");
    cpu_surface_eager_note(0x02A30480, 0); REQUIRE(!s_cpu_surface_eager_dirty, "unbound target does not mark");
    cpu_surface_eager_note(0x02A30480, 0xffffffffu); REQUIRE(!s_cpu_surface_eager_dirty, "unresolved target does not mark");
    cpu_surface_eager_note(0x02A30480, 0xC2A30480); REQUIRE(s_cpu_surface_eager_dirty, "listed bound write marks");
    cpu_surface_eager_submitted(); REQUIRE(!s_cpu_surface_eager_dirty, "submission clears the mark");
    /* 3. The next draw's targets, chosen as capture chooses them. */
    REQUIRE(!cpu_surface_eager_state_writes(NULL), "no state writes nothing");
    clear_targets(); target(0, 0x02A30480, FB); REQUIRE(cpu_surface_eager_state_writes(&next), "slot 0 frame-buffer target");
    clear_targets(); target(3, 0x02A30480, HB);
    REQUIRE(!cpu_surface_eager_state_writes(&next), "inactive D offset does not delay a single-target batch");
    next.color_target = 0x17;
    REQUIRE(!cpu_surface_eager_state_writes(&next), "three active targets leave D inactive");
    next.color_target = 0x1F;
    REQUIRE(cpu_surface_eager_state_writes(&next), "D is active in the four-target draw");
    clear_targets(); target(0, 0x02A30480, 0); REQUIRE(!cpu_surface_eager_state_writes(&next), "a slot without colour DMA is not a target");
    clear_targets(); target(0, 0x02A30480, FB); next.color_target = 2;
    REQUIRE(!cpu_surface_eager_state_writes(&next), "colour target B: slot 0 is not written");
    clear_targets(); target(1, 0x02A30480, FB); next.color_target = 2;
    REQUIRE(cpu_surface_eager_state_writes(&next), "colour target B writes slot 1");
    clear_targets(); target(0, 0x01A80500, FB); REQUIRE(!cpu_surface_eager_state_writes(&next), "unlisted target");
    /* 4. Submission decision. */
    reset("02A30480"); s_cpu_surface_eager_dirty = 1; target(0, 0x02A30480, FB);
    REQUIRE(!cpu_surface_eager_wanted(0), "boundary waits while the next draw still writes the surface");
    REQUIRE(cpu_surface_eager_wanted(1), "idle pump submits regardless of the next draw");
    clear_targets(); target(0, 0x00A80500, FB);
    REQUIRE(cpu_surface_eager_wanted(0), "boundary submits once the next draw leaves the surface");
    s_cpu_surface_eager_dirty = 0; REQUIRE(!cpu_surface_eager_wanted(0) && !cpu_surface_eager_wanted(1), "nothing written, nothing submitted");
    s_cpu_surface_eager_dirty = 1; recording = 1;
    REQUIRE(!cpu_surface_eager_wanted(0) && !cpu_surface_eager_wanted(1), "never while render_frame replays");
    recording = 0; current_thread = 7; state_reads = 0;
    REQUIRE(!cpu_surface_eager_wanted(0) && !cpu_surface_eager_wanted(1) && !state_reads, "only the renderer service thread decides");
    current_thread = 2; s_cpu_surface_thread = 0;
    REQUIRE(!cpu_surface_eager_wanted(1), "no service thread yet: no submission");
    s_cpu_surface_thread = 2; cpu_surface_eager_submitted();
    REQUIRE(!cpu_surface_eager_wanted(1), "submitted batch clears the request");
    cpu_surface_eager_configure(""); s_cpu_surface_eager_dirty = 1; current_thread = 7;
    REQUIRE(!cpu_surface_eager_wanted(1), "feature off: no decision, no owner read");
    puts("PASS: eager CPU surface submission: list, marks, next-draw targets, guards");
    return 0;
}
