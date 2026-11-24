# SPURS kernel notification

Set `SPURS_KERNEL_NOTIFY=1` to make the coordinator of a SPURS instance react
at once when work arrives. The switch is off by default. The coordinator thread
gives each free lane to a workload.

Without the switch the coordinator runs one pass, then sleeps 1 ms. With the
switch it waits on a waitable timer until one of two events:

- A kick. `spurs_kick()` sends one when a lane returns and when a ReadyCount
  call stores a nonzero value. A kick makes the timer signal at once. The pass
  after a kick admits only workloads that have a ready count or a signal.
- The fallback deadline, `SPURS_KERNEL_POLL_MS` after the last timed pass (1 ms
  in a normal build). The pass after a deadline is a timed pass. It admits
  every eligible workload. A kick does not move the deadline.

## Clock and timer

The first version of the switch kept the deadline in `GetTickCount64` and
waited with `SleepConditionVariableSRW`. On a Windows desktop at its default
timer resolution both parts failed:

- `GetTickCount64` advances with the system tick. Measured step on one host:
  15.55 ms, with and without `timeBeginPeriod(1)`. A deadline of 1 ms was
  reached at a tick edge only. The timed pass ran about every 15.6 ms.
- A condition-variable timeout ends at the next system tick when no process
  raises the timer resolution. Measured on the same host: a timeout of 1 ms
  took 15.59 ms. Windows ignores a `timeBeginPeriod` request of a process
  whose window is minimized or covered.

Now the deadline uses `QueryPerformanceCounter`, and the wait uses a
manual-reset waitable timer that `CreateWaitableTimerExW` creates with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`. The coordinator arms the timer for
the deadline and waits with the instance lock released. A kick arms the timer
to expire at once. To arm the timer replaces a signal that is pending, so one
arm ends one wait. The source is `libs/spurs/spurs_notify_wait.h`.

## Limits

- Windows 10 1803 and later end a wait on a high-resolution timer below the
  system tick. Measured on one Windows 11 host: a deadline of 2 ms took
  about 2 ms. An older system falls back to a standard timer. There the
  fallback pass keeps the cadence of the system tick.
- The POSIX shim has no high-resolution timer. Its waits end at the
  granularity of the shim.
- If the timer cannot be created, the coordinator keeps the reference cadence
  of `Sleep(1)`.

## Cost

One timer handle for each SPURS instance that runs with the switch. Each kick
is one `SetWaitableTimer` call.

## Measure

MotorStorm with lifted SPU jobs, first version of the switch (system-tick
clock): no change inside the noise. Vehicle wait 29.57 ms against 28.29 ms per
frame, PVS wait 18.06 ms against 19.62 ms, median frame 165.9 ms against
166.5 ms. This version has no game measurement.

## Check

Run `python tests/check_spurs_notify_wait.py`. It compiles the production wait
header with an authored harness and asserts the unit conversion of the clock,
the period, that the clock has no system-tick step, that the timer ends a wait
at a 2 ms deadline below the tick, that a wake from another thread ends a long
wait, and that a new arm replaces a pending signal. It checks the wiring in
`libs/spurs/cellSpurs.c` and rejects one mutant per rule. Then it runs
`tools/test_spurs_scheduler.py` without mutants: the real coordinator in
notification mode, with a faked performance counter for the deadline.
