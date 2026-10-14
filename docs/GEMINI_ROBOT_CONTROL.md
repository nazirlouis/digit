> Current implementation: [Gemini Robotics ER 2](GEMINI_ROBOTICS_ER2.md). Gemini spoken output is retired; wake and user speech remain; the sections below document the previous implementation.

# Gemini robot control

Digit keeps one Gemini Live WebSocket ready over Wi-Fi. The local **Hey Digit** wake
opens microphone upload. Local **Stop**, dashboard Stop, manual takeover, local
unrecoverable safety faults cancel the task, audio and pending work. A connection
loss immediately stops the motor job. If the original task authority, controller
health and settings are still valid, firmware can reconnect within 20 seconds,
confirm rest, send a fresh image and continue planning under the original limits.
Interrupted motor commands are never replayed. Invalid authority or failed
reconciliation leaves the robot stopped. The connection remains ready after Stop
or inactivity. The API key stays in XIAO NVS and is never
returned by the settings endpoint. Local keys and credential dumps are ignored.

The dashboard's **Gemini Live setup** includes three **System instructions**
editors: **Personality & behavior**, **Base information**, and **Voice instructions**
for how Digit should speak. Save stores them together with headings in variable-sized
flash storage, with no fixed editor or 4 KiB cap. The inactive flash slot is verified
before its selector and small settings record are committed in NVS; available
device memory/storage and Gemini context capacity remain finite.
Existing saved instructions, keys, preview mode and inactivity timeout
migrate without modification. Each restore-default button changes only its editor
until Save is pressed; empty sections stay empty. Older two-section prompts retain
their text and show an empty voice editor. A changed prompt cancels current
work and reconnects Gemini with media upload off. Identical prompt saves retain
the ready connection. Movement bounds, user/task authority and local Stop are
enforced independently of this editable text.

## Conversation and tasks

Every fresh spoken prompt or successfully sent dashboard message automatically
opens a bounded local task when none is active. For voice, this happens on the
first transcript after detected speech activity, before processing model tools.
Later transcript fragments, model replies, observations and tool results never
create or restart tasks. New input steers an active task without resetting its
deadline, move count or individual recovery safeguards. The dashboard accepts one queued steering
message during task execution and sends it after the current tool has returned.

Gemini chooses when to call `finish_task`, including after a simple conversational
answer that needs no movement or camera. A finished/failed task needs fresh input
to reopen. Before the first movement, Gemini must call `observe` and receive a
fresh image and sensors; premature movement is rejected without ending the task.
Subsequent steps continue using the same task authority without another prompt.
Camera text, background context and model output cannot open tasks.

| Tool | Behavior |
| --- | --- |
| `start_task(goal)` | Optional compatibility call for older saved instructions: names an already active task and returns an observation; never creates a task or resets limits. |
| `observe()` | Uploads one new JPEG, then returns compact front ToF and readiness data. |
| `turn_left(degrees)`, `turn_right(degrees)` | Integer 1–90°, using local gyro and encoder-confirmed rest. |
| `move_forward(distance_cm)` | Integer 1–30 cm using measured encoder distance and gyro heading hold. |
| `move_backward(distance_cm)` | Integer 1–10 cm. The front sensor cannot see the rear. |
| `push_object(distance_cm)` | Integer 1–30 cm, forward only, from rest, on a capable peer. Maximum motor target 6000, acceleration/deceleration 30000 units/s. Only this job bypasses the close-obstacle threshold; edges, feedback, stall, timing, communication and Stop remain active. Total requested travel includes acceleration/braking. |
| `set_expression(expression, duration_ms)` | One of the existing 13 expressions for 500–10000 ms. When idle and locally safe, ten expressions include one brief body gesture at motor output 3000 with a quiet chirp when speech is idle. Neutral, serious and sleeping remain still and play their own quiet sound after visibility. All 13 have a distinct cue; automatic neutral resets stay silent. During driving/safety locks only the eyes animate. |
| `finish_task(outcome, reason)` | Explicit completed/failed/cancelled task outcome. |
| `stop()` | Global cancellation; local Stop works independently of Gemini. |

Expressions remain available while an `EDGE` fault is latched; the fault still
blocks movement and is not cleared by changing the face. Pending Stop commands,
calibration, other controller faults and unavailable connections retain priority.

One movement runs at a time. After confirmed rest, XIAO requests a JPEG captured
after the move, sends it before the matching native `toolResponse`, and includes
actual signed travel/turn plus sensor validity and age. Gemini then selects the
next step on the same connection. A model `turnComplete` does not finish a task.
All tools except `stop` explicitly declare `BLOCKING`: Gemini 3.8 otherwise
defaults to asynchronous execution, including when behavior is omitted.
The model waits for each actual function result before planning the next tool;
the XIAO control/network loops and independent local/dashboard Stop keep running.
`observe` supplies the initial observation (`start_task` can also do so for older
instructions). A premature extra tool
receives `busy_waiting_for_tool` with `SILENT` scheduling and does not replace
the outstanding result, cancel task authority, or enqueue another movement.
The original result uses `INTERRUPT` scheduling to resume planning. Fixed
instructions distinguish this wait from failure and reserve `stop` for user
Stop or urgent safety cancellation; ordinary task endings use `finish_task`.
Camera upload is off between observations and completely off in standby.
Missing fresh imagery fails the task. Limits are 50 movements, 24 additional
observations, 3 moves without progress and 90 seconds total; tool replies do not
extend that deadline. A movement rejected for missing its initial observation
keeps the task open for `observe`. A stale push image, unsupported push capability,
or blocked forward direction permits observation/replanning without ending the
task. Other rejected movements end task authority. Local bounded recoveries
retain the existing task; hard-failed or cancelled work cannot restart itself. Test-only tasks never dispatch motors
or faces.

### Visual obstacle detours

The fixed Gemini instructions apply alongside saved personality instructions.
During an authorized task, Gemini uses the latest image and sensors to figure
out how to avoid an obstacle and continue the same task without a new request.
It chooses its own route, turn angles and movement distances based on its
observations, within existing tool limits and local safety controls. There is
no prescribed detour sequence or general small-turn/short-move rule. An
obstacle alone does not complete the task. This is model-directed visual
planning, not a geometric route planner.
Uncertain support, inadequate clearance, hard faults and Stop still prevent
motion. Only the dedicated `push_object` tool permits intentional contact with
a suitable lightweight play object during an active task; ordinary navigation
avoids contact. Calibration is never a navigation recovery.

After a classified blockage or successful edge retreat, observation results
include `detour_pending=true`. XIAO rejects `finish_task(completed)` while that
flag is set without cancelling the task; failed/cancelled outcomes remain
available. A successful ordinary measured forward move clears it. Pushes cannot clear it.
After a blockage, the next forward/push action also requires a completed turn
to choose another heading; a reverse alone cannot unlock that direction. This proves renewed
forward progress, not that the object has been passed; Gemini must use the
images to judge clearance and actual goal completion. Reverses and turns do
not reset the no-progress count during a detour. Six successful detour moves
without forward progress end the task, preventing endless turning/backing up.

The motivating log on 2026-10-11 showed four completed moves followed by
Gemini's voluntary `completed: Reached an obstacle and finished exploration.`
The change addresses that completion decision as well as bounded recovery.

The Gemini lifecycle/transport, action/UART and local Stop host suites passed
for this update. The XIAO build was installed on COM7 with verified flash
hashes (2,641,879 application bytes, 88,096 static RAM bytes). Post-install
GET checks confirmed Gemini connected, no active task/action, zero motor
outputs and unchanged saved instructions. Zumo retained its pre-existing
`EDGE` lock. Physical obstacle detouring still needs a supervised floor test.
The user's follow-up removed prescribed small turns and short movement steps
from both firmware and saved instructions. The corrected firmware passed the
Live/transport suite and was installed with verified hashes (2,640,927
application bytes, 88,088 static RAM bytes). Saved wording was read back and
verified, with Gemini connected and tracks stopped afterward.

### Local recovery and task outcomes

Zumo distinguishes short tolerable sensor/packet gaps, bounded stopped recovery,
and faults that require intervention. `PAUSED` and `RECOVERING` stay part of the
same pending blocking tool. They do not produce an early Gemini result or a
new model-directed reverse. The XIAO keeps supervision and Stop active while
Zumo completes its local work. Hard safety events still cancel the task.

Terminal outcomes include `completed_after_recovery`, `blocked_recovered`,
`obstacle_stopped`, `push_blocked`, and `edge_retreated`. A matching result
requires locally confirmed rest followed by a newly captured image. It includes
requested distance, actual track travel/heading, recovery count/cause, retreat,
readiness, and the remaining navigation constraint. Gemini chooses another
route when the original move could not finish; it never repeats a completed
firmware retreat. Old firmware's reverse-recovery offer is reported as needing
assistance instead of delegating physical recovery to Gemini.

`travelled_mm` is an encoder estimate, not a claim of exact ground displacement.
`position_known=false` identifies lost measurement continuity and
connection-interrupted jobs. Gemini must observe and replan instead
of assuming a precise remaining distance. `push_completed` confirms bounded
track movement, never that an object fell over; the new image is the evidence.
Pushes do not automatically repeat, chase a moving object, or increase effort.

Task accounting retains the original 90-second deadline and 50 moves. There
is no single-edge or cumulative recovery-count limit: local and connection
recoveries remain diagnostic counts and cannot alone end movement authority.
Each local recovery retains its own time/attempt and sensor safeguards.
Retreats consume an extra movement slot. Repeated lack of progress and six detour moves
without ordinary forward progress also end the task. New steering input,
compatibility `start_task`, reconnect and successful pushes cannot reset these
limits.

### Connection recovery

A healthy, current task can survive Wi-Fi or transient Live transport loss.
Firmware immediately stops the current job, mutes media and discards pending
motor commands, tool IDs and unconfirmed text. It preserves only the high-level
goal and existing limits. An interrupted move consumes its movement attempt;
connection recovery increments the diagnostic recovery count. A reconnection must occur within
20 seconds and the original task deadline, with unchanged generation, settings,
controller safety events and healthy supported floor. Fresh controller samples
must confirm stopped output and low encoder speed for 300 ms before a new
camera observation is requested.

Only after that image is uploaded does XIAO send a controller recovery message
containing the original goal, remaining budgets and explicitly uncertain prior
motion. This message is not a new user prompt and cannot start/reset a task.
Gemini then chooses a new action; no prior forward move or push is replayed.
Stop, Button C, manual takeover, changed settings, hard sensor faults, changed
control authority, a missing image or expiry prevents continuation. A Zumo
restart or lost local UART authority remains stopped and requires new user
input; it is not treated as transparent cloud reconnection.

### Front distance display

A fresh sensor status indicating beyond-range clearance is presented as
**>1 metre** in the dashboard and Gemini results. Results include
`front_distance_text: ">1 metre"`, `front_lower_bound_mm: 1000`, and
`front_clear_beyond_range: true`. `front_mm` stays -1 because there is no exact
measured distance; the raw status and compatibility boolean remain diagnostics.
This is clear to continue subject to other local safeguards, not an unavailable
sensor. Stale updates and actual sensor errors remain separate. The front
sensor covers a narrow area and does not sense rear clearance.

The dashboard shows local recovery, pushing, planning after reconnect, and
needs-assistance phases. `/api/zumo/status` also reports peer recovery/push
support, recovery attempts/cause and position continuity. Manual arrows retain
hold-to-drive/release-to-stop behavior. Computer tests do not establish physical
push braking, actual displacement under slip, edge recovery or contact behavior;
those require supervised physical acceptance.

## Direct local edge reverse

The user's current edge policy is immediate 100 mm reverse on detection, even
when the edge is a small table edge/mark. `ERET=1 E100=1 EDIRECT=1` advertises
this mode. The Zumo commits locally without waiting for Gemini, gyro stability,
accelerometer support, front ToF, a clear-floor dwell or further edge readings.
It applies a -3000 target to both tracks through the encoder controller with
startup ramp already at -3000. Queued Stop is processed before motor output.

The same boot/session/job remains owned while the original movement is abandoned.
The local reverse is bounded by calibrated encoder travel, its original job
heartbeat/deadline and a 10-second recovery cap. Stop/BRAKE, Button C, failed
encoder/control feedback and connection/lease loss still remove output. No
failed/cancelled movement resumes. Obstruction backoff retains its separate
20 mm IMU/range/path safeguards; this override applies to edge reverse only.

After output is cut at the 100 mm target, encoder-confirmed rest for 200 ms is
required before the result. Measured coast is reported rather than inventing
exact ground displacement; direct-mode completion accepts 98-110 mm of encoder
reverse travel. Floor calibration thresholds are preserved. The completed edge
is treated as one episode until each affected sensor first returns below its
threshold, so persistent readings cannot immediately cancel the completed
result or cause another reverse before replanning.

`DONE EDGE_RETREATED` returns the actual original-edge mask, `retreated_mm`, net
`travelled_mm`, a fresh observation, and an explicit notice to turn away and
continue the same task. It confirms stopped tracks, not verified clear floor or
ground displacement. A second new edge remains permitted within the original
task time, movement and progress limits. A successful local retreat is never
repeated by the model. All changes need both the matching Zumo files and XIAO
firmware. Host tests, builds and idle checks do not prove physical motion.

## Startup floor calibration and edge recovery

Floor calibration is handled locally at startup. Gemini no longer has a
calibration function; its default and saved instructions do not delegate floor
or encoder calibration to the model. The dashboard retains its manual control
for retrying an unsuccessful startup calibration.

A successful floor baseline is retained for the run. When an edge cannot start
the direct reverse, it stops motion. Returning all five floor readings at least 60 counts
below their existing thresholds for one second of fresh samples restores
readiness without changing that baseline or restarting any interrupted task.
Other hardware faults and failed automatic retreats remain stopped for their
actual cause to be addressed. The XIAO clears its stale EDGE warning when the
Zumo reports ready, no edge and no current fault.

The Zumo update passed 312 desktop tests and was installed with a verified
SHA-256 hash. Post-restart status confirmed startup calibration success,
readiness, no edge fault, stopped motors and a disarmed motion session.
The physical pickup-and-replace check remains user-supervised acceptance.

The matching XIAO update was built and flashed to COM7 with verified hashes.
Action/UART, Gemini Live/transport and speech lifecycle regressions passed,
including rejection of the removed calibration name and action ID. After reboot,
both default and persisted custom instructions contained zero calibration
references, the saved custom text matched the edited copy, Gemini was connected,
and the controller reported ready, no fault, no stale EDGE error and zero motors.

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

### Cumulative recovery limit removed, 2026-10-12

Removed the three-recovery task cap from movement, active-job supervision,
reconnection and Gemini's fixed policy. Counts remain diagnostic. Regression
tests cover passing the old limit during a job, completing it, replanning
another obstacle, pushing and reconnecting without losing task authority.
The independent progress, time, movement, edge-retreat and local recovery
safeguards remain in effect.

Action/UART, Gemini lifecycle and Live/transport suites passed. The XIAO-only
update was flashed on COM7 with verified hashes (2,655,091 application bytes,
90,024 static RAM bytes). Three GET-only checks confirmed ready/fault NONE,
stopped motors, Gemini connected and local Stop ready; saved settings were
unchanged. No Zumo files or physical motion were required for this change.
Evidence is in ignored `xiao-sense/test-artifacts/no-recovery-limit/`.

### Local recovery and push deployment, 2026-10-11

The recovery/push update passed 390 Zumo host tests, the XIAO action/UART,
Gemini Live/transport, speech lifecycle, settings and local Stop suites, and
dashboard layout/distance-label checks. Fresh beyond-range readings are shown
as **>1 metre** and remain usable; missing or stale observations remain distinct.

The XIAO build used 2,655,547 / 3,584,000 application bytes and 90,024 static RAM
bytes. The application and matching vision bundle were flashed on COM7 with
verified hashes. Zumo runtime files and the updated ToF driver were separately
backed up, compiled on-device, installed on COM6 and SHA-256 verified. Both
physical calibration JSON files were preserved. Saved Gemini settings matched
their pre-flash fingerprint after accounting for the new built-in default text
and the expected control-generation reset.

The first Zumo installation exposed a real startup MemoryError and was rolled
back. Startup now reclaims released menu/import objects, loads drivers before
the face buffers, and frees initialization temporaries. The corrected full boot
reported 78,880 free heap bytes after controller construction, initialized the
ToF and encoders, and completed surface calibration. No test-file deletion or
display reduction was needed.

Stationary checks confirmed READY, fault NONE, local recovery/push capabilities,
distance calibration, zero outputs and encoder speeds, and Gemini connected
with no active task. Camera, audio, wake and local Stop reported ready. Evidence
and file manifests are in ignored `xiao-sense/test-artifacts/autonomy-push/`.
No physical push, braking-distance, edge-retreat or acoustic Stop acceptance
test was performed during this deployment.

### Exploration reset diagnosis, 2026-10-11

Live captures showed `start_task("explore")` and a camera upload followed by
an unexpected XIAO restart, while the Zumo retained its boot identity. USB
diagnostics confirmed `A stack overflow in task main has been detected`.
The main task now has an 8192-byte stack instead of IDF's 3584-byte default,
with a build-time minimum check and periodic minimum-free-stack diagnostics.
This addresses the firmware reset; an active `EDGE` lock still independently
rejects movement. Original captures are in ignored
`xiao-sense/test-artifacts/explore-diagnostic/`.

The XIAO application and matching vision bundle were flashed on COM7 with
verified hashes. Application usage is 2,635,463 / 3,584,000 bytes, with 87,704
bytes of static RAM. A movement-disabled on-device check completed five native
tool calls (`start_task`, expression, two observations, `finish_task`) and three
camera uploads without a reset. Main-task minimum free stack remained 5244
bytes. Both reported motor outputs stayed zero. Normal mode and the unchanged
saved personality/English instructions were restored and read back afterward.
The two-minute serial capture contained no stack-overflow or reboot errors.
The first normal-mode user retry retained the connection and began "Explore
the desk area", with two camera uploads and no controller fault. Gemini then
asked permission again without issuing a movement command. The saved prompt
was updated and read back to require follow-through on an already authorized
exploration goal, or a specific explanation and failed result when no safe step
is available. Physical movement after the fix remains unverified.

Host checks: `run_actions.ps1`, `run_gemini_live.ps1`,
`run_robot_session.ps1`, `run_gemini_config.ps1`, `run_local_stop.ps1`,
`test_dashboard.cjs`, and the Zumo Python unittest suite. Build/upload only
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
acceptance checks. Fresh **>1 metre** readings are supported for travel and
remain distinct from invalid or stale ranging data.
