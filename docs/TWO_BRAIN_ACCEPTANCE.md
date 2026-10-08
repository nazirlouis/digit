# Digit two-controller acceptance

Digit keeps MicroPython on the Zumo. The XIAO owns mission decisions, vision,
voice and audio. The Zumo owns motor execution, encoder feedback, gyro control,
local sensor safety and motion completion. The wire contract is in
`TWO_BRAIN_PROTOCOL.md`.

## What a software pass does and does not establish

Desktop tests cover state transitions, cancellation, bad packets, retries,
expired leases, sensor failures and rendering bounds. A successful firmware
build verifies the XIAO target compiles. A USB transfer and hash comparison
verify installed files. Idle hardware checks verify startup, communication and
diagnostics without commanding movement.

None of those checks establishes loaded stopping distance, track slip, physical
turn accuracy or reliable edge avoidance. These require supervised tests on the
actual robot. Do not describe those physical properties as verified from mocks.

## Startup and idle acceptance

The 2026-10-07 installation exposed two startup failures: an unbounded audio
I2C wait in the installed SDK, and repeated control-port binding after only the
camera-stream server failed. The fixes and their limits are documented under
XIAO scheduling and startup recovery in `TWO_BRAIN_PROTOCOL.md`.

Before motion testing, confirm these separately from physical acceptance:

| Check | Evidence |
| --- | --- |
| Build-local I2C backport | Configuration succeeds against the expected SDK block; compile output includes `digit_patches/i2c_master.c`; installed SDK source stays unchanged |
| Early motor communication | Serial startup reports Zumo UART initialization before audio initialization; idle controller status remains available |
| Completed XIAO startup | Audio, voice, dashboard and camera statuses are reported; no repeating startup watchdog trace |
| Partial HTTP startup recovery | If the stream server fails, control routes remain registered once and retries occur at most once per second; no repeated port-80 address-in-use loop |
| Idle robot state | Motor outputs remain zero, boot/capability fields are current, and sensor/loop diagnostics are populated |
| Internal memory headroom | Record free internal bytes and largest block after both HTTP servers start, then with dashboard/camera/voice/vision active; a successful server-start message alone is insufficient |

Run the desktop XIAO suite from `xiao-sense` with
`powershell -File tests/run_actions.ps1`. It includes
`tests/test_i2c_timeout_patch.py`: exact-block replacement and timeout/reset
logic, refusal of changed or ambiguous SDK source, refusal to overwrite the
SDK input, and idempotent generation. These tests verify backport generation;
they do not simulate the ESP32 I2C peripheral or prove hardware recovery.
Rebuild and perform an idle startup check after changing the SDK or backport.

A subsequent startup completed but left only 1139 internal bytes free and a
544-byte largest block; HTTP requests then timed out despite both servers
reporting started. The three 8192-byte wake/speech compute stacks now reside in
PSRAM to recover internal capacity without shrinking their stacks. Verify the
result on hardware, including a voice command and the saved-audio diagnostic
path, rather than treating the theoretical 24 KiB relocation as measured free
headroom. Flash/NVS operations must remain on internal-stack tasks.

### Installed validation, 2026-10-07

- XIAO environment `xiao_esp32s3_sense_idf` built successfully and its application
  was flashed on COM7 with esptool hash verification. The installed application
  SHA-256 is `964c5c6fbd34f96795e2752baa2c7a5bbeed514bfdad9f1dc39109cd745d67a9`.
- Nine matching Zumo files were backed up, transferred, read back and hash
  checked on COM6. Module imports and stationary startup calibration passed.
  The installed manifest is in
  `xiao-sense/test-artifacts/two-brain/installed-manifest.json`.
- All 150 Zumo desktop tests passed. The XIAO action/link/speech/cue suite,
  build-local I2C patch tests and dashboard/layout tests passed. Simulated
  cancellation coverage includes 360 mission Stop cases.
- Idle HTTP checks reported both controllers connected, motion protocol
  supported, calibrated encoder feedback, valid ToF readings around 263 mm,
  zero motor outputs, audio ready, camera ready and speaker volume 100%.
  Five complete JPEG frames were received from the stream.
- XIAO internal free memory was 23019 to 24219 bytes after startup, with a
  17408 to 18432-byte largest block. During stationary camera-only person/object detection it
  remained about 22775 bytes free. Both models produced fresh results at about
  200 ms inference time while dashboard and UART status remained responsive.
- The longest reported Zumo control gap was 30 ms during these stationary
  checks. Display updates occasionally exceeded the requested cooperative
  slice budget (reported maximum 17534 us); these are measured soft budgets,
  not a hard real-time or physical stopping guarantee.
- USB maintenance was tested on the new Zumo runtime: motors off precedes
  watchdog disable, raw REPL remained usable beyond the watchdog period, and
  the normal program restarted afterward.
- Saved-audio diagnostics exercised the PSRAM speech worker without dispatching
  actions. Stop and Come say hello clips were recognized correctly. One older
  synthetic Find a way out clip was classified as What's this; a second
  synthetic voice produced no result. Live maze-command recognition remains
  an acceptance item; do not report all voice commands as verified from this run.

### Initial supervised movement

The user confirmed a clear, level floor, enabled motor power and placed a
visible target in front. The first action did not move: it exposed a shared
clock race on XIAO that produced `JOB_ACK_TIMEOUT` before transmitting ARM.
Desktop tests reproduced the race before the fix and passed afterward. The
final application above includes that correction and removes HTTP calls to
the UART poller, leaving a single packet writer.

After a stationary surface recalibration, the supervised retry progressed
through ARMING, CALIBRATING and RUNNING. The same job drove continuously from
about 345 mm toward the wall. Measured left/right velocities were closely
matched (for example 1678/1704 and 1629/1657 counts/s) while controller motor
outputs differed, demonstrating active feedback rather than equal PWM alone.
Zumo reported WALL_BRAKING at 80 mm before the test supervisor sent Stop after
about one second of observed motor output. The subsequent range was 51 mm,
motor outputs were zero and filtered encoder velocities were 19/18 counts/s.
The user reported "Straight and smooth."

This first run verifies initial travel and cancellation; Stop interrupted the
natural wall-completion sequence, so it does not alone verify WALL_STOPPED.

Subsequent user-supervised wall/turn checks did reach WALL_STOPPED at 50 mm and
48 mm with zero motor outputs before the scan began. Front MEASURE completed,
then a left TURN executed locally. An initial test guard incorrectly stopped
same-job reverse correction after an approximately 99-degree turn; the guard
was corrected to follow the job identity. In the completed retry, the heading
changed from 146.4 to 240.7 degrees, or 94.3 degrees for the requested 90. The
mission then advanced through another stationary measurement to its next
turn job, and the test sent Stop before that turn's motor output began. This
is within the existing 8-degree turn tolerance, not evidence of exact 90-degree
accuracy. Further precision tuning should use repeated loaded trials.
The user confirmed the completed turn and clean stop looked good.

Edge latches during setup were confirmed by the user to follow lifting or
repositioning Digit. Stationary recalibration restored readiness; edge safety
was not disabled. Full multi-junction maze/battery completion, repeated turns
in both directions, supported edge-fixture stopping and live maze-command
recognition remain acceptance items.

Relevant logs are in `xiao-sense/test-artifacts/two-brain/`.
The original XIAO application backup is `xiao-before.bin` there. The original
Zumo snapshot is `zumo-controller/backups/2026-10-07-205840-before-two-brain`;
the immediate pre-install snapshot is
`zumo-controller/backups/2026-10-07-213633-two-brain-install`.

## Staged physical acceptance

Use the dashboard Stop and physical Button C throughout. Start on a supported
test surface; a desk edge must not be the first edge-detection experiment.

| Test | Record | Passing behavior |
| --- | --- | --- |
| Raised tracks | Requested and measured encoder speeds, motor direction | Correct signs, independent speed correction, no movement after Stop |
| Loaded straight travel | Heading error and lateral drift over a measured distance | Continuous motion, corrections without repeated starts/stops |
| 90 degree turns both ways | Actual final angle across repeated trials | Settled completion, repeatable angle, no extra turn on a retried command |
| Wall approach at 50 mm | Final front clearance, speed, battery condition | Braking begins before the target; completion only after physical settling |
| Wall approach at 100 mm | Same measurements | Greeting/battery arrival remains distinct from a maze wall |
| Edge fixture | Detection and stop latency with the chassis safely supported | Local stop regardless of XIAO workload; explicit recovery required |
| Sensor interruption | Last local sample age and exact terminal result | Missing/invalid data cannot be interpreted as clear space |
| XIAO interruption | Lease expiration and stop distance | No indefinite drive and no automatic restart after recovery |
| Vision/audio/display load | Maximum control gap, overruns, display slice time | Control remains within its configured timing limits or stops explicitly |
| Maze | Job/event history and observed path | Forward until wall, compare front/left/right, choose longest valid gap |
| STOP ALL during every phase | Active job, generation and remaining cues | Motion and pending character work cancel; old commands cannot restart them |

Set numerical acceptance tolerances using measured performance before raising
speed. The existing 3000 minimum is a motor-effort constraint, not mm/s. Current
encoder calibration was measured with raised tracks; do not infer physical
speed or loaded braking distance from it. Front clearance also needs the sensor
mounting offset measured on Digit.

## Rollout and rollback

1. Verify the active project is `xiao-sense` and environment is
   `xiao_esp32s3_sense_idf` before building or uploading.
2. Back up the actual installed Zumo files and XIAO application while idle.
3. Run both controller test suites and dashboard tests, then build the XIAO.
4. Install the matching Zumo modules together, compare transferred bytes/hashes,
   and restart while stationary. Upload the matching XIAO application.
5. Verify boot identity, capabilities, idle motor outputs and sensor diagnostics.
   New autonomous missions require the new protocol and encoder calibration;
   they must never silently fall back to the old repeatedly refreshed drive.
6. Perform supervised physical acceptance separately from installation.

Rollback restores the saved matching application and Zumo files while stopped.
Never restore only one board and assume autonomous protocol compatibility.

The reactive maze policy does not maintain a map or guarantee escape from loops
and dead ends. It does not inspect directly behind the arrival heading. A
single front ToF sensor cannot certify clearance around the entire turning
footprint.
