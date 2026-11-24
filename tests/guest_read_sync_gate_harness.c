static unsigned long s_table_reads;
/* Fixture for check_guest_read_sync_gate.py. It compiles the production
 * header and source with an authored handler. A wrapper counts the reads of
 * the page table. */

/* The check compiles a copy of the production header in which the page
 * reader has the name `real_page_read`. The production loop calls
 * `vm_read_sync_page`, the wrapper here. The wrapper counts the read and
 * calls the production reader. */
static unsigned char real_page_read(unsigned index);
static unsigned char vm_read_sync_page(unsigned index);
#include "ps3emu/guest_read_sync.h"

#include <stdio.h>
#include <string.h>

static unsigned long s_handler_calls;
static uint32_t s_last_address, s_last_bytes;

static void handler(uint32_t address, uint32_t bytes, void *opaque)
{
    (void)opaque;
    s_handler_calls++;
    s_last_address = address;
    s_last_bytes = bytes;
}

/* The wrapper for the call in the production loop. */
static unsigned char vm_read_sync_page(unsigned index)
{
    s_table_reads++;
    return real_page_read(index);
}

static int s_failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

int main(void)
{
    /* 1. No range is marked: no handler call and no table read. */
    CHECK(vm_read_sync_any == 0);
    vm_read_sync_set_handler(handler, NULL);
    vm_cpu_read_sync(0x10000, 4);
    vm_cpu_read_sync(0x20000, 0x1800);
    vm_read_sync_dispatch(0x30000, 4);
    CHECK(s_handler_calls == 1);          /* the direct dispatch call */
    CHECK(s_table_reads == 0);
    s_handler_calls = 0;

    /* 2. A rejected mark does not set the flag. */
    CHECK(vm_read_sync_mark_range(0xFFFFFFF0, 32) == -1);
    CHECK(vm_read_sync_any == 0);
    vm_cpu_read_sync(0x40000, 4);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 0);

    /* 3. One mark. A read of that page calls the handler one time. */
    CHECK(vm_read_sync_mark_range(0x50000, 16) == 0);
    CHECK(vm_read_sync_any != 0);
    vm_cpu_read_sync(0x50020, 4);
    CHECK(s_handler_calls == 1);
    CHECK(s_last_address == 0x50020 && s_last_bytes == 4);
    s_handler_calls = 0;

    /* 4. The flag is set, so a read of a free page reads the table only. */
    s_table_reads = 0;
    vm_cpu_read_sync(0x60000, 4);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 1);

    /* 5. The multi-page loop. The first page is free, the second is marked.
     * The handler runs one time with the full range. */
    CHECK(vm_read_sync_mark_range(0x71000, 4) == 0);
    s_table_reads = 0;
    vm_cpu_read_sync(0x70F00, 0x200);
    CHECK(s_handler_calls == 1);
    CHECK(s_table_reads == 2);
    CHECK(s_last_address == 0x70F00 && s_last_bytes == 0x200);
    s_handler_calls = 0;

    /* 5b. Two marked pages in one range: the handler runs one time, for the
     * first marked page. The loop stops after the dispatch. */
    CHECK(vm_read_sync_mark_range(0x90000, 0x2000) == 0);
    s_table_reads = 0;
    vm_cpu_read_sync(0x90000, 0x2000);
    CHECK(s_handler_calls == 1);
    CHECK(s_table_reads == 1);
    CHECK(s_last_address == 0x90000 && s_last_bytes == 0x2000);
    s_handler_calls = 0;

    /* 6. A range of three free pages reads three pages and stops. */
    s_table_reads = 0;
    vm_cpu_read_sync(0x80000, 0x2FFF);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 3);

    /* 7. A range of zero bytes does nothing. */
    s_table_reads = 0;
    vm_cpu_read_sync(0x71000, 0);
    /* With zero bytes the end page is the page before the address. At address
     * zero that page wraps to the top of the table. */
    vm_cpu_read_sync(0x72000, 0);
    vm_cpu_read_sync(0, 0);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 0);

    /* 8. A range past the top of the address space does nothing. */
    vm_cpu_read_sync(0xFFFFFFF0, 17);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 0);
    /* The last 16 bytes are one page of the space. */
    vm_cpu_read_sync(0xFFFFFFF0, 16);
    CHECK(s_handler_calls == 0);
    CHECK(s_table_reads == 1);

    if (s_failures) {
        fprintf(stderr, "guest_read_sync_gate: %d failures\n", s_failures);
        return 1;
    }
    printf("guest_read_sync_gate: PASS\n");
    return 0;
}
