# Gemini robot control

Digit keeps one Gemini Live WebSocket ready over Wi-Fi. The local **Digit** wake
opens microphone upload. Local **Stop**, dashboard Stop, manual takeover, local
safety faults and connection loss cancel the task, audio and pending work.
The connection remains ready after Stop or inactivity; failures reconnect idle
without replaying previous motion. The API key stays in XIAO NVS and is never
returned by the settings endpoint. Local keys and credential dumps are ignored.

## Conversation and tasks

A greeting needs no robot task. Gemini replies and may choose silent animated
eyes. A navigation request starts a bounded task through `start_task(goal)`.
This requires recent direct user speech. Subsequent tool calls use that task's
authority, so each completed step can continue without another user prompt.
Camera text and background media never authorize a task.

| Tool | Behavior |
| --- | --- |
| `start_task(goal)` | Starts a 90-second task and returns an initial observation. |
| `observe()` | Uploads one new JPEG, then returns compact front ToF and readiness data. |
| `turn_left(degrees)`, `turn_right(degrees)` | Integer 1–90°, using local gyro and encoder-confirmed rest. |
| `move_forward(distance_cm)` | Integer 1–30 cm using measured encoder distance and gyro heading hold. |
| `move_backward(distance_cm)` | Integer 1–10 cm. The front sensor cannot see the rear. |
| `set_expression(expression, duration_ms)` | One of the existing 13 expressions for 500–10000 ms; silent and independent of motion/speech. |
| `finish_task(outcome, reason)` | Explicit completed/failed/cancelled task outcome. |
| `stop()` | Global cancellation; local Stop works independently of Gemini. |

One movement runs at a time. After confirmed rest, XIAO requests a JPEG captured
after the move, sends it before the matching native `toolResponse`, and includes
actual signed travel/turn plus sensor validity and age. Gemini then selects the
next step on the same connection. A model `turnComplete` does not finish a task.
Camera upload is off between observations and completely off in standby.
Missing fresh imagery fails the task. Limits are 12 movements, 24 additional
observations, 3 moves without progress and 90 seconds total; tool replies do not
extend that deadline. A rejected movement ends task authority. Failed/cancelled
work cannot restart itself. Test-only tasks never dispatch motors or faces.

ToF is a narrow front reading, not a distance to every visible object. Invalid
or stale readings and no-return are distinct. Zumo owns edge/Button C protection,
encoder/gyro faults, heartbeat expiry, job deadlines and braking. Gemini cannot
set raw motor power or change those protections. Manual driving retains its
existing hold-to-drive/release-to-stop behavior.

The dashboard contains manual arrows, vision buttons beside them, task status,
silent face controls, the speaker volume slider and Gemini configuration.
Fixed routines, Turn with person, recording/replay/speaker diagnostics and
emotion sounds have been retired. The local wake acknowledgement is visual.

## Distance calibration: the final powered step

Encoder **velocity** calibration is not encoder **distance** calibration.
`distance_calibration.py` loads measured floor travel from a separate Zumo-root
`distance_calibration.json`. Missing or invalid measurements leave forward and
backward tools disabled; no wheel diameter or gear ratio is guessed. Gyro turns
are independent of this distance scale.

With motor power available and a supervised safe floor area:

1. Stop Digit; start the GET-only count capture:
   `python zumo-controller/tools/calibrate_distance.py start`.
2. Drive straight forward manually along a ruler, stop, and measure the actual
   travel (at least 100 mm). Keep the robot clear of obstacles and edges.
3. Save the measured run, for example with your measured distance substituted:
   `python zumo-controller/tools/calibrate_distance.py finish --measured-mm 300`.
4. Inspect and install the generated `distance_calibration.json` on the Zumo,
   then restart. It is ignored by Git, along with the temporary count capture.
5. Validate 10/30 cm forward, 5/10 cm reverse and 30/90° turns on the actual
   surface. Confirm braking, obstacle/edge Stop, local spoken Stop and takeover.

The helper never commands motors or installs values. Repeat measurement if a
track slips or the heading changes substantially. Passing computer tests,
flashing XIAO, installing Zumo files and idle sensor checks do not establish
physical travel accuracy.

## Verification

Host checks: `run_actions.ps1`, `run_gemini_live.ps1`,
`run_gemini_speech.ps1`, `run_gemini_config.ps1`, `run_local_stop.ps1`,
`test_drive_layout.cjs`, and the Zumo Python unittest suite. Build/upload only
the `xiao_esp32s3_sense_idf` environment and its matching vision bundle. Zumo
Python files require a separate USB installation, backup and hash verification.

Google's [Live API tool documentation](https://ai.google.dev/gemini-api/docs/live-api/tools)
describes native function responses and continuation. XIAO handles actual tool
execution, matching IDs, lifetime duplicate detection and session cancellation.

### Installation and stationary checks, 2026-10-10

The final XIAO application and matching vision bundle were flashed on COM7 with
verified flash hashes. App usage is 3,400,359 / 3,584,000 bytes (94.9%); static
RAM is 86,264 bytes (26.3%). Zumo modules were backed up, installed separately
on COM6 and hash-verified. No distance measurement was installed.

All tool/protocol/conversation/local Stop/configuration/dashboard host suites
passed, along with 249 Zumo Python tests. The served dashboard was checked for
the retained volume slider and removed follow control. Idle device telemetry
reported both motors zero, fresh ToF, ready encoders/gyro, expression support,
distance calibration disabled and the saved Gemini key still configured.

A quiet test-only session uploaded 480,320 audio samples, no camera frames,
and ended after inactivity while retaining connection count 1. After the final
Stop-priority update, a separate test-only session uploaded 191,840 samples,
no camera frames, and explicit Stop retained the same connection; counters
stayed unchanged afterward and there were no dropped audio frames or errors.
Serial health checks stayed clean. No assistant speech was produced in these
quiet checks. Motors were not tested with power off; audible replies, spoken
wake/Stop, physical travel/turning, braking and distance calibration remain
supervised powered acceptance steps. Actual encoder-to-distance calibration
is deliberately the final step.

### Powered encoder and motor calibration, 2026-10-10

With the user confirming raised tracks and motor power, each track was pulsed
forward and backward separately. Encoder signs matched the commands, the idle
track remained at zero, and the user confirmed both physical directions. The
factory encoder sign handling was retained.

Measured velocity samples at motor powers 3000 and 6000, in both directions,
updated `zumo-controller/encoder_calibration.json`. The file was backed up,
installed on the Zumo and SHA256-verified. A subsequent closed-loop raised-track
check at targets 3000, -3000 and 4000 matched the two tracks within 1%; measured
speed was within 3.5% of each target. These checks establish raised-track speed
control, not floor traction or turning accuracy.

The user then measured 12.5 inches (317.5 mm) of total straight floor travel,
confirming no slipping or repositioning. The same-boot encoder delta was 3434
left / 3360 right counts, including an early edge stop followed by continued
straight travel. This gives 10.815748 left / 10.582677 right counts per mm.
The measured `distance_calibration.json` was installed separately and its SHA256
matched the local file. Fresh post-restart Wi-Fi telemetry reported distance
calibration enabled, motors zero, ready and no fault. The distance file, count
capture and calibration evidence remain ignored by Git.

Two automatic 10 cm forward jobs completed with encoder estimates of 106 and
105 mm. The user measured the forward travel as a little less than 4.5 inches
and accepted the result; this is an approximate ruler check, not a precise
error measurement. The 5 cm reverse job completed at an estimated 49 mm and
the user accepted its floor behavior. Native action logs confirmed
`TRAVEL_DONE`, encoder-confirmed rest, no fault and post-move observations.
The forward estimates show about 5–6 mm of overshoot at this starting speed.

A user-requested 20-degree turn also completed with `TURN_DONE`; its physical
angle was not measured. Larger travel/turn accuracy, physical stopping margins
and spoken Stop during powered movement still require separate supervised
acceptance checks. Fresh sensor no-return is supported for travel; it is
distinct from invalid or stale ranging data.
