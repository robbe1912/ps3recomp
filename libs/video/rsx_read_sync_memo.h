/* Ranges that the CPU surface bind marked for the read sync.
 *
 * vm_read_sync_mark_range sets one mark per 4 KB page, the flag, and (by its
 * hook) the page bytes of the fast memory path of the PPU. The state stays:
 *   - no function clears a mark (ps3emu/guest_read_sync.h);
 *   - ppu_fm_page_or only sets bits, and ppu_fm_window_end keeps the bits of
 *     a page that has PPU_FM_K_SYNC (runtime/ppu/ppu_loader.cpp).
 * So a second mark of a range writes only values that are set, with one
 * exception: the hook gives the page before the range only PPU_FM_K_READ, and
 * ppu_fm_window_end can clear that bit. When that page is inside a marked
 * range, the bit stays too. The hook is installed before the first mark
 * (ppu_fm_arm at boot), as ps3emu/guest_read_sync.h asks.
 *
 * A bind runs for each draw that uses a surface, so the second and later
 * marks of one surface repeated in each frame. The memo holds the ranges that
 * one instance marked. A range is skipped when the memo holds it and the page
 * before it is inside a marked range. Another range can take the slot of a
 * range; then the next bind marks again, as without the memo.
 *
 * One instance belongs to one thread. Nothing clears an instance. */
#ifndef PS3RECOMP_RSX_READ_SYNC_MEMO_H
#define PS3RECOMP_RSX_READ_SYNC_MEMO_H

#include "ps3emu/guest_read_sync.h"

#ifndef RSX_READ_SYNC_MEMO_ENTRIES
#define RSX_READ_SYNC_MEMO_ENTRIES 1024u   /* a power of two */
#endif
#ifndef RSX_READ_SYNC_MEMO_PAGES
#define RSX_READ_SYNC_MEMO_PAGES (1u << 20)   /* the 4 KB pages of the guest space */
#endif

typedef struct rsx_read_sync_memo_entry {
    uint64_t bytes;      /* 0: the slot holds no range */
    uint32_t address;
} rsx_read_sync_memo_entry;

typedef struct rsx_read_sync_memo {
    rsx_read_sync_memo_entry entry[RSX_READ_SYNC_MEMO_ENTRIES];
    unsigned char inside[RSX_READ_SYNC_MEMO_PAGES / 8u];   /* 1: the page is in a marked range */
} rsx_read_sync_memo;

static unsigned rsx_read_sync_memo_slot(uint32_t page)
{
    return (unsigned)(((page * 2654435761u) >> 12u) & (RSX_READ_SYNC_MEMO_ENTRIES - 1u));
}

static int rsx_read_sync_memo_inside(const rsx_read_sync_memo *memo, uint32_t page)
{
    return (memo->inside[page >> 3] >> (page & 7u)) & 1u;
}

static void rsx_read_sync_memo_record(rsx_read_sync_memo *memo, uint32_t address, uint64_t bytes)
{
    const uint32_t first = address >> 12;
    const uint32_t last = (uint32_t)(((uint64_t)address + bytes - 1u) >> 12);
    rsx_read_sync_memo_entry *slot = &memo->entry[rsx_read_sync_memo_slot(first)];
    slot->address = address;
    slot->bytes = bytes;
    for (uint32_t page = first; page <= last; page++) {
        unsigned char *byte = &memo->inside[page >> 3];
        const unsigned char bit = (unsigned char)(1u << (page & 7u));
        if (!(*byte & bit)) *byte = (unsigned char)(*byte | bit);
    }
}

/* The result of vm_read_sync_mark_range(address, bytes): that call, or 0 for
 * a range that this memo marked and can skip. */
static int rsx_read_sync_memo_mark(rsx_read_sync_memo *memo, uint32_t address, uint64_t bytes)
{
    const uint32_t first = address >> 12;
    rsx_read_sync_memo_entry *slot = &memo->entry[rsx_read_sync_memo_slot(first)];
    if (slot->address == address && slot->bytes == bytes && slot->bytes
        && (!first || rsx_read_sync_memo_inside(memo, first - 1u)))
        return 0;
    const int result = vm_read_sync_mark_range(address, bytes);
    if (!result) rsx_read_sync_memo_record(memo, address, bytes);
    return result;
}

#endif
