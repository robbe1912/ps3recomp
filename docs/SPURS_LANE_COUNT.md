# SPURS lane count override

`SPURS_LANES=N` replaces the lane count that the game requests for each SPURS instance. `N` is a decimal number from 1 to 6. The Cell design has 6 SPUs for an application. Any other value, or no setting, keeps the count of the game.

The game sees the effective count. `spurs_initialize_common` writes it to the instance and `cellSpursGetInfo` returns it. The log line of the initialization shows both counts:

```
[cellSpurs] Initialize "<prefix>" ea=0x........ nSpus=6 requested=5 (real BE instance + kernel poll)
```

## When more lanes help

More lanes shorten a wait only when jobs are ready and no lane is free. Each lane is one host worker thread. A lane that has no job sleeps.

## Check

`python tests/check_spurs_lane_count.py` compiles `libs/spurs/spurs_lane_count.h` with `tests/spurs_lane_count_harness.c`, asserts the effective count for each kind of value, checks the wiring in `libs/spurs/cellSpurs.c`, and rejects one mutant per rule.
