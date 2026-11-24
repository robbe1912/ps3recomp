# Guest read sync gate

`vm_cpu_read_sync(address, bytes)` runs before each CPU read of guest memory.
It reads one byte of the page table `vm_read_sync_pages` for each 4 KiB page of
the range. When a byte is set, it calls `vm_read_sync_dispatch`.

No caller marks a page in most titles. The flag `vm_read_sync_any` removes the
table reads for that case.

## Rules

- `vm_read_sync_mark_range` sets `vm_read_sync_any` to 1 when it marks at least
  one page. It sets the flag after the pages.
- No function clears a mark. No function clears the flag.
- `vm_cpu_read_sync` returns at once when the flag is zero.
- A rejected mark (zero bytes, or a range past the top of the address space)
  does not set the flag.
- A range of zero bytes does nothing at any address.

## Check

Run `python tests/check_guest_read_sync_gate.py`.

The check compiles the production header and the production source with the
fixture `tests/guest_read_sync_gate_harness.c` and an authored handler. The
check renames the page reader of one header copy so that the fixture can count
the table reads. The loop is the production text.

The fixture asserts:

- zero handler calls and zero table reads when no range is marked;
- one handler call after a mark, with the full range as arguments;
- the multi-page loop: a free page before a marked page, and two marked pages
  in one range.

It rejects five mutants:

- gate inverted: return on a set flag
- gate removed
- flag not set by the mark
- loop continues after the dispatch
- zero bytes read the table
