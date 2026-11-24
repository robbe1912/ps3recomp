# SPURS wait priority

Set `SPURS_WAIT_PRIORITY=1` to give a free SPU lane first to the workload that
a PPU thread waits on. The switch is off by default. The value must be exactly
`1`.

## Behavior

A PPU thread that waits on a job list cannot continue until the workload of
that job list ran. The coordinator thread of a SPURS instance gives each free
lane to the first workload that passes the admission checks. It tests the
workloads from a round-robin cursor. The workload of the waited job list can
then wait behind other workloads, and the PPU thread waits longer.

With the switch, the port registers a callback:

```c
typedef uint64_t (*spurs_wait_priority_fn)(void *ctx);
void spurs_wait_priority_set(spurs_wait_priority_fn fn, void *ctx);
```

The callback returns the job list address that a PPU thread waits on, or 0
for none. A null function removes the callback. The declaration is in
`libs/spurs/spurs_wait_priority.h`. The definition is a strong symbol, because
a weak symbol does not link on COFF.

One pass of the coordinator does the following:

1. It calls the callback one time. The callback runs under the instance lock.
   It must return at once and it must not call a SPURS function.
2. It finds the first workload of the instance whose data is that job list
   address, or no workload.
3. For each free lane, the scan tests that workload at its first position.
   The other positions test each workload once, in the order of the cursor.
   The cursor steps 16 times for 16 workloads. When there is no wanted
   workload, the scan is the old scan of 16 positions.

The admission checks do not change. The wanted workload must pass each check:
enabled, state 2, in use, a nonzero priority for the lane, contention below
the maximum, a policy module, and a ready count, a signal or a timed pass.
When the wanted workload fails a check, the lane goes to the first workload of
the cursor that passes.

## Record

With the switch, the coordinator prints this line to stderr at most one time
per second:

```
[SPURS_WAIT_PRIORITY] waits=N boosted_jobs=M
```

- `waits`: the number of free lanes that a pass gave while a PPU thread waited
  on a workload.
- `boosted_jobs`: the number of jobs that a pass admitted at the first position
  of the scan.

## Cost

Without the switch, a pass does one environment read at its start, from a
cache. With the switch and no callback, a pass does one atomic load. With a
callback, a pass does one call and at most 16 reads of the workload registry.

## Risk

The workload of the waited job list runs before other workloads. The other
workloads run later. A job list that more than one PPU thread waits on at the
same time gives one job list address, the last one that the port stored.

## Check

`python tests/check_spurs_wait_priority.py` compiles the header with an
authored harness and rejects 22 mutants of the header. It checks the wiring of
the pass in `libs/spurs/cellSpurs.c` and rejects 10 mutants of the pass. It
compiles `libs/spurs/cellSpurs.c` syntax-only.
