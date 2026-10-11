# Digit two-brain motion protocol (MOTION=1)

The XIAO owns mission decisions, perception and speech. The MicroPython Zumo
owns persistent motor execution, encoder feedback, gyro heading, local edge
checks, ToF stopping, braking, stopped confirmation and deadlines.

The current Gemini tool layer exposes bounded TURN and measured TRAVEL jobs,
with independent silent SCENE expressions. Older DRIVE/SPIN/MEASURE commands
below remain wire compatibility features; their former dashboard routines are
retired. See [Gemini robot control](GEMINI_ROBOT_CONTROL.md).

## Wire contract

Legacy commands retain their format. `STATUS` advertises `MOTION=1 BOOT=xxxxxxxx SESSION=<high-water>`
and, after any session has been opened, emits a second checksummed M2 snapshot.
The legacy status also includes GAP, OVERRUN, DSUS, TL and TR so timing and
encoder targets can be inspected while idle without arming a motion session.
`TRAVEL=1 DCAL=<0|1> EXPR=1 CL=<signed count> CR=<signed count>` advertise travel,
measured distance readiness, bounded expressions and raw encoder counts.
Boot is a random eight-character lowercase hexadecimal token generated on boot.
Autonomous missions must require MOTION=1, ENC=1 and VOICE=1; they must not silently
fall back to the legacy repeatedly refreshed SAFE/LOOK commands.

Request:

```
<wire-id> M2 <boot> <session> <job-id> <verb> [arguments] *<crc>
```

`wire-id` is 1..65535. Session is 1..4294967295, strictly increasing during one
Zumo boot. Job IDs are 1..65535 and strictly increase within a session; ARM and
HB use job 0. A new XIAO process learns the current session high-water mark from
STATUS and chooses the next session. Session overflow requires a Zumo reboot.
Maximum line length is 159 bytes excluding newline.

CRC is CRC-16/CCITT-FALSE (polynomial 0x1021, initial 0xffff, no final xor or
reflection), four hexadecimal digits. Input is ASCII from `M2` through the last
argument, excluding the space and asterisk before the CRC. The wire ID is not
included. Both requests and snapshots have CRCs.

| Verb | Arguments | Meaning |
| --- | --- | --- |
| ARM | none | Enter autonomous mode while ready, encoder-stopped, with gyro and encoder feedback. New session required after cancellation/fault. |
| HB | counter | Refresh the 750 ms supervisor lease only when the counter strictly increases (1..4294967295, reset on ARM). Replayed/duplicate counters do not extend it. Does not extend any job deadline. No reply. |
| DRIVE | left right acceleration stop_mm max_ms | Persistent forward drive. Targets 3000..3200, acceleration 500..6000, stop 50 or 100 mm, deadline 100..60000 ms. Equal targets enable gyro heading hold. |
| UPDATE | revision left right | Update current DRIVE target without restarting its ramp/deadline. Positive revision increases; duplicate identical revision is harmless. |
| TURN | degrees acceleration max_ms | Relative gyro turn, positive left; -180..180 degrees, acceleration 500..6000, deadline 100..30000 ms. |
| TRAVEL | signed_mm speed acceleration max_ms | Encoder distance with gyro heading hold; -100..300 mm, nonzero, speed 3000, acceleration 500..6000, deadline 100..15000 ms. Requires TRAVEL=1 DCAL=1 and encoder/gyro feedback. |
| SPIN | max_ms | One continuous in-place gyro revolution, positive left; fixed fast opposite track targets, deadline 1000..30000 ms. Requires SPIN=1. |
| MEASURE | max_ms | Wait for encoder-confirmed rest, then return median of three new valid ToF samples; deadline 100..5000 ms. |
| ACK | none | Acknowledge current job snapshot; job ID must match. Stops terminal retransmission, does not erase result. No reply. |
| CANCEL | none | Immediately stop and close the session; job 0 or current job accepted. |

Snapshot/reply/event:

```
<wire-id> M2 <boot> <session> <job-id> <state> <reason> <distance_mm> <heading_tenths> <range_age_ms> GAP=<ms> OVERRUN=<count> DSUS=<us> TL=<cps> TR=<cps> *<crc>
```

Wire 0 denotes asynchronous events. States: DISARMED, ARMED, CALIBRATING,
RUNNING, BRAKING, SETTLING, DONE, FAULT. Successful terminal reasons are
WALL_STOPPED, TURN_DONE, TRAVEL_DONE, SPIN_DONE and MEASURED. Snapshots also carry
`TRAVEL=<signed actual mm>` for the measured travel result. Protocol rejection is `<wire-id> ERR
M2_<reason>` and cannot start movement. Snapshot distance is the current ToF
distance except MEASURED, which retains the median and its age. Heading is
integrated gyro yaw, not an absolute localization estimate. Job identity binds
MEASURED to its request; its three samples are taken after settling.

Exact job retries return the retained current snapshot without restarting the
job. Reusing a job ID with different arguments is rejected. Old session/job IDs
are rejected. Terminal events repeat every 250 ms until ACK and remain visible
in STATUS until another job is accepted. Late UPDATE for a matching braking
or finished drive job returns its current snapshot without changing targets
or resuming it.

Legacy STOP, BRAKE or a manual motion takeover closes the autonomous session.
STOP has no CRC requirement and remains available even after corrupt transport.
Local edge, Button C, encoder errors, invalid/stale range, missed heartbeat,
motion deadline and excessive active control-loop delay stop locally. No job
automatically resumes after any fault, Stop, reboot or session closure.

Legacy BRAKE closes the session immediately but decelerates normally; the
local ToF hard guard and stale-range checks remain active throughout that
brake. STOP and faults remove motor output immediately.

## Motion behavior and limits

TRAVEL loads a separate `distance_calibration.json` measured on the physical
surface; missing/invalid calibration rejects the job. It uses both encoders,
signed wrap-safe counts, gyro heading hold, local braking and encoder-confirmed
rest. Reverse uses signed targets and heading correction. Direction disagreement,
track mismatch, overshoot, loss of encoder/gyro/range feedback, edge, Button C,
supervisor expiry and job deadlines stop locally. Actual distance calibration
is deferred until the final powered floor step; velocity calibration does not
substitute for it.

`SCENE <expression> <caption> [duration_ms]` accepts an optional 500..10000 ms
duration when EXPR=1. Expressions can render during an autonomous motion job
without changing its targets or speech. Safety/manual control has priority.
SCENE_VISIBLE acknowledges actual first-frame rendering. All expressions are
silent; the previous sound coordination is retired.

`SPIN=1` advertises the separate one-revolution job. Its local profile uses
opposite signed track targets of 6000 with acceleration/deceleration 30000,
tapering to the reliable 3000 effort near the endpoint while still moving.
One stationary gyro calibration and one unwrapped 360-degree reference cover
the whole motion. It never chains two turns, normalizes 360 to zero, reverses,
or rests between segments. The final brake begins only after a complete
revolution; small braking yaw is retained in the snapshot. Rotation beyond
390 degrees faults with `SPIN_OVERSHOOT` even during final settling.
`SPIN_DONE` requires
gyro settling plus zero output and encoder-confirmed rest for 150 ms.
Fresh ToF status 2 (no return) is usable while rotating, without inventing a
distance. Sensor failures, stale observations, sustained unreliable status,
and a valid distance below 50 mm still stop the job locally. Starting at 50 mm
is permitted; local floor-edge, Button C, heartbeat and timeout guards remain
active throughout calibration, rotation and settling.
If a matching `SCENE HAPPY FOUND` is already visible when SPIN is accepted,
the Zumo holds that exact scene through calibration, rotation and settling.
It preserves the applied frame and entry clock; it does not send another
scene or extend the brief sound. DONE, cancellation and faults release the
matching face hold. A replacement face/scene owns its display immediately
and cannot be overwritten by a late spin cleanup. Plain SPIN with another
face does not select or hold a happy scene.

The normalized target is not millimetres/second; calibrated encoder counts/sec
map it to motor effort, preserving the user's 3000 minimum sustained effort.
Straight drives use gyro yaw correction in addition to independent track speed
feedback. Initial gyro bias is sampled while stationary; it is periodically
refreshed. Smooth ramp outputs pass through lower efforts during startup/braking.

Wall approach predicts braking lead from observed ToF closing speed, current
effort/ramp braking time and range age. No wheel dimensions are guessed. A
three-sample median rejects isolated distance jumps in the velocity estimate.
If braking stops more than 10 mm short of the requested clearance, the same
bounded job resumes its approach; it never declares a distant wall reached.
Six unsuccessful correction approaches fault rather than loop indefinitely.
WALL_STOPPED occurs only after output is zero and both measured encoder speeds
remain <=50 counts/sec for 150 ms. The independent obstacle guard is strictly
below 50 mm for every automatic job, regardless of its arrival target. An
obstacle stop is not a successful arrival. Manual drive/turn has no front-distance
guard. Validity and
freshness are checked locally during braking too. Physical final clearance must
be measured under real load; the software profile is not proof of a 5 cm stop.

Control-gap instrumentation records gaps >20 ms; an active job stops after a
gap >150 ms. Main also feeds an RP2040 hardware watchdog after a successful
controller tick, so a wedged interpreter/driver resets with motors off at boot.
Exiting main, including USB Ctrl-C, calls motors.off first, then clears the
RP2040 WATCHDOG_CTRL ENABLE bit through its atomic clear alias. This pauses
the watchdog only for the stopped maintenance/raw-REPL state, allowing USB
updates. The register/bit and clear-alias mapping follow the official SDK:
[watchdog registers](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2040/hardware_regs/include/hardware/regs/watchdog.h),
[address map](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2040/hardware_regs/include/hardware/regs/addressmap.h),
[watchdog_disable implementation](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/hardware_watchdog/watchdog.c).
Face rendering is cooperative when the new display implementation is present.
`SCENE_VISIBLE` is a separate acknowledgement from accepting SCENE; sound can
wait until the matching scene's first frame has completed transfer.

The forward ToF cannot establish full side/rear turning clearance. Gyro and
encoders do not eliminate track slip. These limits require physical acceptance,
including battery/load variations and induced communication/sensor failures.

## XIAO scheduling and startup recovery

The UART service runs in a dedicated task without the mission/UI mutex. It is
started before audio initialization so an optional peripheral cannot prevent
motor Stop/status communication. Its requested 5 ms period is rounded to at
least one FreeRTOS tick (10 ms with the current 100 Hz configuration). Only
mission polling refreshes the separate 600 ms supervisor lease; a healthy UART
task cannot indefinitely sustain a stalled mission by sending heartbeats.

Only this task calls `zumo_link::poll`; HTTP handlers queue requests without
writing UART packets. Shared clocks are sampled after acquiring the status
lock. In particular, the UART deadline clock is sampled after receive parsing:
a mission can queue a request while receive parsing runs, and comparing that
new timestamp against an earlier unsigned clock falsely produces a timeout.
Regression tests reproduce queued-ARM, renewed-supervisor and snapshot-age
races; the first was also observed during the initial supervised hardware run.

An observed startup hang exposed ESP-IDF 5.5.0's unbounded synchronous I2C NACK
bus-busy wait. `xiao-sense/cmake/bounded_i2c.cmake` generates a build-local copy
of `i2c_master.c` using `tools/patch_i2c_timeout.py`; the installed SDK remains
unchanged. This backports the upstream elapsed-tick timeout, timeout status,
hardware state-machine reset and loop exit from
[Espressif's I2C master driver](https://github.com/espressif/esp-idf/blob/master/components/esp_driver_i2c/i2c_master.c),
verified 2026-10-07. The exact known source block must occur once; an SDK change
or ambiguous match fails configuration and requires explicit review. This
addresses the observed synchronous wait, not every possible peripheral fault.

Dashboard and camera-stream HTTP servers retain independent startup state. A
failed stream startup keeps the working control server and its Stop endpoint;
retrying does not bind port 80 again or duplicate its routes. Startup retries
are spaced one second apart, with internal-memory diagnostics on failure.
The stream server uses a 4096-byte stack, while the control server retains its
larger stack. Route-registration failures clean up the affected server before
retrying. Autonomous motion does not depend on either HTTP server or Wi-Fi;
manual drive retains its short hold-to-drive lease.

Wake inference, speech sample assembly and speech recognition each retain an
8192-byte stack allocated explicitly in PSRAM. This relocates 24 KiB of stack
capacity while keeping their task-control blocks in internal RAM; it does not
reduce stack sizes. Their model/partition initialization remains on the
internal-stack startup task, and the worker loops must not acquire flash/NVS
erase/write responsibilities. The UART, audio/I2S, vision model loader and HTTP
control tasks retain internal stacks. This follows the SDK's explicit
capability-based task API and
[external RAM restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/external-ram.html).
Tasks created with the capability API require its matching deletion API on
failure paths. Internal free bytes and largest free block still need hardware
measurement under active dashboard, camera, voice and vision load.
