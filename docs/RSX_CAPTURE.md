# Bounded RSX capture events

Existing `RSX_CAP=start:count:stride` snapshots keep their schedule. An optional
once-only burst shares the output numbering and records its reason and
presenting flag in each manifest. No guest input or rendering state is changed.

Set `RSX_CAP_ARM_FILE` to a new process-local marker path and `RSX_CAP_BURST`
to a count from 1 through 120 (default 64). Creating the marker arms the next
presented batches with recorded operations. Empty and offscreen-only batches
do not spend the burst. Removing/recreating the marker cannot arm another
burst in the same process. Sparse and burst overlap produces one capture.

Alternatively, with capture enabled, set `RSX_CAP_PAD_MASK=0x10` to trigger
the burst when `cellPad` consumes a PAD_FILE command containing that mask.
The notification happens once when the command is accepted, before held
polls are delivered. A shared atomic latch transfers it to the render thread;
the event does not manufacture an input. Other masks, disabled capture and
subsequent events cannot restart a completed burst. This identifies an input
event, not a particular menu or a guarantee that an asset has loaded.

Run `test_rsx_capture_schedule` with `PS3RECOMP_BUILD_TESTS=ON`. The authored
fixture exercises the actual selector and cross-translation-unit event latch,
including late arming, overlap, empty/offscreen filtering and unique indices.
It does not establish game correctness or capture-free performance. Readback
and file output perturb timing; use bounded counts and a separate normal run
for performance measurements.
