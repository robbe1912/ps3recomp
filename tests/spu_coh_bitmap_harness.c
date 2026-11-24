/* Fixture for check_spu_coh_bitmap.py. It compiles the production header and
 * source of the SPU lock-line coherence module and checks the inline bitmap
 * test against them. */
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

/* `spu_coh_notify_write*` wakes the holder through this function. The fixture
 * reserves lines without a context, so the function must not run. */
static int s_wake_calls;
void spu_ch_wake(spu_context *ctx) { (void)ctx; s_wake_calls++; }

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

/* The fixture uses only the bitmap and the armed flag. It passes no context. */
static int line_of(uint32_t addr) { return (int)(addr >> SPU_COH_LINE_SHIFT); }

static void check_free_line(uint32_t addr)
{
    CHECK(line_of(addr) < (int)(1u << 25));
    CHECK(spu_coh_bitmap_test(addr) == 0);
    CHECK(spu_coh_is_reserved(addr) == 0);
}

static void check_reserved_line(uint32_t base)
{
    for (uint32_t i = 0; i < 128u; i++) {
        CHECK(spu_coh_bitmap_test(base + i) == 1);
        CHECK(spu_coh_is_reserved(base + i) == 1);
    }
    /* The two neighbour lines stay free. */
    check_free_line(base - 1);
    check_free_line(base - 128);
    check_free_line(base + 128);
    check_free_line(base + 255);
    /* The line is one bit at the low end of its byte. */
    {
        uint32_t line = base >> SPU_COH_LINE_SHIFT;
        unsigned bit = (unsigned)(line & 7);
        unsigned expect = (unsigned)(1u << bit);
        CHECK((g_spu_coh_bitmap[line >> 3] & expect) == expect);
        if (bit < 7)
            CHECK((g_spu_coh_bitmap[line >> 3] & (expect << 1)) == 0);
        if (bit > 0)
            CHECK((g_spu_coh_bitmap[line >> 3] & (expect >> 1)) == 0);
    }
}

int main(void)
{
    /* Before the first GETLLAR a set bit is not reported. The armed check
     * stands before the bitmap lookup. */
    {
        uint32_t line = 0x40001000u >> SPU_COH_LINE_SHIFT;
        unsigned char saved = g_spu_coh_bitmap[line >> 3];
        g_spu_coh_bitmap[line >> 3] = (unsigned char)(1u << (line & 7));
        CHECK(spu_coh_bitmap_test(0x40001000u) == 1);
        CHECK(spu_coh_is_reserved(0x40001000u) == 0);
        g_spu_coh_bitmap[line >> 3] = saved;
    }

    spu_coh_reserve(NULL, 0x40000000u);
    check_reserved_line(0x40000000u);

    /* The low bit of a byte and the high bit of a byte. */
    spu_coh_reserve(NULL, 0x00010000u + 128u * 3);
    check_reserved_line(0x00010000u + 128u * 3);
    spu_coh_reserve(NULL, 0x7FFFFF00u);
    check_reserved_line(0x7FFFFF00u);

    /* The top line of the 32-bit space. Its last byte is 0xFFFFFFFF. */
    spu_coh_reserve(NULL, 0xFFFFFF80u);
    check_reserved_line(0xFFFFFF80u);
    check_free_line(0xFFFFFF7Fu);

    CHECK(s_wake_calls == 0);

    if (s_failures) {
        fprintf(stderr, "spu_coh_bitmap: %d failures\n", s_failures);
        return 1;
    }
    printf("spu_coh_bitmap: PASS\n");
    return 0;
}
