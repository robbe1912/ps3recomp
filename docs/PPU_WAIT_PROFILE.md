# PPU wait profile

Set `PPU_WAIT_PROFILE=1` to record the wall time of guest waits. It is off by
default. The record is one line per call of five blocking syscalls:
`sys_cond_wait`, `sys_semaphore_wait`, `sys_mutex_lock`,
`sys_event_queue_receive` and `sys_event_flag_wait`. A call that returns at
once also writes a line.

    [PPU_WAIT_PROFILE] tid=<thread> kind=<name> id=<object> lr=<guest call site> started=<ticks> finished=<ticks> frequency=<ticks per second>

`started` and `finished` are QPC ticks on Windows and monotonic nanoseconds on
other systems. `frequency` converts them. The line holds no result. Take the
duration as `finished - started`. Group by `kind`, `id` and `lr` to name the
owner of a wait. A begin on a thread with an open begin replaces that begin.

The switch adds two clock reads and one stderr line per wait. Use it to find
the waits, then run a timing without it.

The source is `runtime/syscalls/ppu_wait_profile.h`. A syscall wrapper calls
`PPU_WAIT_PROFILE_BEGIN(ctx, kind, id)` before the wait and
`ppu_wait_profile_end()` after it. The wait body moves to a static
`<name>_impl` function, so each early return stays inside the bracket.

Run `python tests/check_ppu_wait_profile.py`. It compiles the production
header with an authored harness. It asserts the default-off silence, the
switch values, the record fields, a measured wait and the replace rule of a
nested begin. It checks the hook wiring of the five syscalls and rejects one
mutant per rule.
