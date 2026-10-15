/* Game-free checks of the selector called by the D3D12 capture producer. */
#include <stdio.h>
#include <stdlib.h>
#include "rsx_capture_schedule.h"
#include "rsx_capture_event.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main(void)
{
    rsx_capture_schedule s;
    rsx_capture_pad_configure(0);
    rsx_capture_pad_notify(0x10);
    CHECK(rsx_capture_pad_take() == 0);
    rsx_capture_pad_configure(0x10);
    rsx_capture_pad_notify(0x4000);
    CHECK(rsx_capture_pad_take() == 0);
    rsx_capture_pad_notify(0x10);
    rsx_capture_pad_notify(0x30);
    CHECK(rsx_capture_pad_take() == 0x10);
    CHECK(rsx_capture_pad_take() == 0);
    rsx_capture_pad_notify(0x30);
    CHECK(rsx_capture_pad_take() == 0x30);
    uint32_t index = 99;
    rsx_capture_schedule_init(&s, 1, 2, 120);
    CHECK(!rsx_capture_schedule_next(&s, 0, 1, 1, &index));
    CHECK(rsx_capture_schedule_next(&s, 1, 1, 0, &index) == RSX_CAPTURE_SCHEDULED && index == 0);
    CHECK(!rsx_capture_schedule_next(&s, 120, 1, 1, &index));
    CHECK(rsx_capture_schedule_next(&s, 121, 1, 1, &index) == RSX_CAPTURE_SCHEDULED && index == 1);
    CHECK(!rsx_capture_schedule_next(&s, 1000, 1, 1, &index));
    CHECK(!rsx_capture_schedule_arm(&s, 0));
    CHECK(!rsx_capture_schedule_arm(&s, RSX_CAPTURE_BURST_MAX + 1));
    CHECK(rsx_capture_schedule_arm(&s, 3));
    CHECK(!rsx_capture_schedule_next(&s, 1001, 0, 1, &index));
    CHECK(!rsx_capture_schedule_next(&s, 1002, 1, 0, &index));
    CHECK(s.burst_remaining == 3);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(rsx_capture_schedule_next(&s, 1003 + i, 1, 1, &index) == RSX_CAPTURE_BURST && index == i + 2);
    CHECK(!rsx_capture_schedule_next(&s, 1006, 1, 1, &index));
    CHECK(!rsx_capture_schedule_arm(&s, 3));
    rsx_capture_schedule_init(&s, 1, 3, 2);
    CHECK(rsx_capture_schedule_arm(&s, 2));
    CHECK(rsx_capture_schedule_next(&s, 1, 1, 1, &index) == 3 && index == 0);
    CHECK(rsx_capture_schedule_next(&s, 2, 1, 1, &index) == RSX_CAPTURE_BURST && index == 1);
    CHECK(rsx_capture_schedule_next(&s, 3, 1, 1, &index) == RSX_CAPTURE_SCHEDULED && index == 2);
    CHECK(rsx_capture_schedule_next(&s, 5, 1, 1, &index) == RSX_CAPTURE_SCHEDULED && index == 3);
    CHECK(!rsx_capture_schedule_next(&s, 6, 1, 1, &index));
    rsx_capture_schedule_init(&s, UINT32_MAX, 2, UINT32_MAX);
    CHECK(rsx_capture_schedule_next(&s, UINT32_MAX, 1, 1, &index));
    CHECK(!rsx_capture_schedule_next(&s, (uint64_t)UINT32_MAX + 1, 1, 1, &index));
    CHECK(rsx_capture_schedule_next(&s, (uint64_t)UINT32_MAX * 2, 1, 1, &index));
    rsx_capture_schedule_init(&s, 0, 0, 0);
    CHECK(rsx_capture_schedule_arm(&s, RSX_CAPTURE_BURST_MAX));
    for (unsigned i = 0; i < RSX_CAPTURE_BURST_MAX; ++i)
        CHECK(rsx_capture_schedule_next(&s, i, 1, 1, &index) == RSX_CAPTURE_BURST && index == i);
    CHECK(!rsx_capture_schedule_next(&s, RSX_CAPTURE_BURST_MAX, 1, 1, &index));
    puts("PASS: cross-TU input event latch, sparse schedule, late finite once-only burst, presented/empty filtering, overlap, unique indices, overflow");
    return 0;
}
