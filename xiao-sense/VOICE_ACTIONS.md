# Digit voice commands and dashboard Stop

## Current three-action release - local, not installed

The current source exposes **Come say hello** (ID 8), **What's this?** (ID 9),
and **Find a way out** (ID 14), plus **Stop / Stop moving** (ID 7).
This replaces the earlier action sets in the historical installation notes
below. The light dashboard and 100% startup speaker volume are included in
this same pending release. Neither controller has been updated for it.


Say **Digit**, wait for his answering voice, then speak one command within six seconds.
The existing custom wake model opens English MultiNet 6. Recognition runs on
the XIAO; no network speech service is used. The dashboard's **Digit actions**
panel runs the same actions as the spoken commands below. **Go to bed** is deferred; no garage model is required for this release.

Commands use uppercase English text, registered through
`esp_mn_commands_add` and applied with `esp_mn_commands_update`. Like the
[ESP32 MultiNet demo](https://github.com/nazirlouis/esp32-multinet-demo/blob/main/main/speech_recognition.c),
the listener accepts a MultiNet `DETECTED` result with a nonempty phrase. There
is no additional application confidence cutoff. Digit also checks the command
ID and starts at most one action per wake. Confidence is reported for diagnosis.
The dashboard reports a recognized command or asks you to retry after an
unmatched command or interrupted audio.

The model bundle's initial command list and runtime registration share
`src/speech_phrases.h`. No phoneme mapping or pronunciation converter is needed.
Adding a phrase requires its text, command ID, and a rebuild/full upload.

When an idle wake has no usable microphone bearing, Digit waits silently for
up to 500 ms before answering. During this window, the direction still must
pass the existing stability check and be no older than 300 ms. A valid bearing
starts one gyro turn (including a rear-facing 180-degree turn), followed by the
answering voice and command listening. If direction remains unavailable, stale
or unstable, Digit answers without turning. Disabled turning, incomplete setup,
playback, motor activity, disconnected or locked Zumo do not get a delayed retry.
Stop, manual takeover and setup changes prevent a pending turn from starting.
The dashboard's voice-direction panel and `/api/voice/status` `turn_reason`
record acquisition, completion, cancellation or the exact skip reason; serial
logs report the same message. Active-action wake takeover remains stop-and-listen.

Installed 2026-10-07 on XIAO COM7 after desktop direction, wake acquisition,
action/mission, speech, UART and dashboard tests passed. The build used
3,534,707 / 3,584,000 application bytes; all five upload hashes verified.
GET-only checks confirmed voice ready, saved direction setup enabled/calibrated,
the new diagnostic field and dashboard display, and zero motors. The existing
Zumo EDGE lock was preserved. No live rear-voice turn was commanded or verified.

## Audio and recognition flow

The ReSpeaker remains the only microphone source. Its 16 kHz, 32-bit stereo
I2S stream is converted to 16-bit mono from the right channel, without extra
gain. Separate feed and detection tasks follow the demo's task structure:
continuous capture, frame assembly, queue fetch, then MultiNet detection.
The ReSpeaker supplies the hardware front end; no ESP-SR software AFE, VAD,
noise suppression, echo cancellation, or automatic gain control runs on the XIAO.
MultiNet receives exactly 512 samples per call (32 ms), assembled in order from
160-sample microphone reads. The model rate and frame size are checked at startup.
ESP-SR is pinned to 2.4.6, matching the working demo; MultiNet is version 6.
This includes the 2.4.5 ESP32-S3 speech/vision coexistence fix.

The live routing audit on 2026-10-05 read left `8/0`, right `7/3`, unpacked
outputs, ASR enabled, ASR gain `1.0`, and AEC bypass `0`. The right channel
therefore carries the auto-selected ASR beam after echo cancellation, without
post-processor noise suppression or AGC. The left post-processed channel has
AGC and its limiter enabled, with stationary/nonstationary noise floors of
`0.15` / `0.51`; those settings do not apply to the right ASR branch. See the
[XMOS output routing and ASR guide](https://www.xmos.com/documentation/XM-014888-PC/pdf/xvf3800_user_guide_v3.2.1.pdf).

The custom **Digit** wake model still runs on the same ReSpeaker stream. Its
existing fixed gain and microWakeWord feature extraction remain part of its
working model input. That branch does not alter the samples sent to MultiNet.
There is one microphone capture task and one wake recognizer. ESP-SR WakeNet
and VADNet are not instantiated or bundled. Playback continues on the same I2S
peripheral so the ReSpeaker can receive its existing echo reference.

At startup, read-only I2C queries cache the ReSpeaker's actual channel routing,
ASR gain, AEC bypass, and post-processor settings. `GET /api/audio/processing`
reports them; unsupported reads return -1. Hardware capabilities alone do not
prove a selected channel uses every processor: the XVF ASR branch bypasses the
post-processor, including its noise suppression and AGC. Reading this endpoint
does not change the microphone configuration or hold the I2C bus during driving.

After the 1.8-second Happy wake voice finishes and a 120 ms echo tail passes, MultiNet
owns the six-second recognition timeout. A stuck voice prompt aborts after
2.5 seconds; Stop during the prompt prevents later listening from starting.
A ten-second wall-clock watchdog only cancels stalled listening. Capture/session
metadata follows every complete model frame, so mixed wake/Stop frames, stale sound,
and results from cancelled sessions cannot start actions. Queue loss cancels
listening. Microphone errors disable voice commands. CPU/cache configuration
matches the demo's 240 MHz, 32 KB instruction cache and 64 KB data cache.
Before allocating the models, ordinary buffers larger than 1 KB prefer PSRAM.
Task stacks and buffers requiring internal/DMA memory retain their required
capabilities. Serial health logs report internal free memory and the largest
internal block separately from PSRAM, so available PSRAM cannot hide a shortage
of memory needed by the dashboard or Wi-Fi.

| Command | Behavior |
| --- | --- |
| Come say hello | Find and center a person, approach in short steps to the existing 100 mm front-ToF target, perform an excited wiggle, then finish with LOVE eyes and a soft affectionate voice. Any detected person counts; no identity recognition. |
| What is this / What's this | Stay still, examine a battery, marker or screwdriver, show a surprised recognition beat, then happy eyes and the object name. Uncertain evidence produces confused eyes and NOT SURE. One result per command. |
| Find a way out | Explore a simple floor course with a marker at the exit. Compare measured openings, move in bounded steps, align with the marker and celebrate at the existing 550 mm ToF threshold. Maximum 18 exploratory advances / 150 seconds; no general maze-solving guarantee. |

The dashboard and dispatcher share readiness checks. Moving actions require
floor calibration, gyro/turn/gesture support, and valid front range younger
than 250 ms. Identification only needs the connected display and object model;
it does not require the person model, gyro, ToF or floor calibration.
The speech registry contains six phrases for these four IDs. Retired IDs
1-6, 10-13, 15 and 16 are rejected without interrupting an active action.
Old Say hello, Find me and Follow me phrases are not aliases. The separate
**Turn with person** camera tool retains its dashboard heartbeat.

### Reliability and character behavior

- Stationary scenes and new motion require a motor STATUS newer than the last
  stop request, at most 300 ms old, with both motors zero. After braking or
  turning, allow 120 ms to settle and use only images captured afterward.
- People require two matching detections out of three at 55% confidence.
  Objects require three out of four at 50%, allowing one brief miss. Evidence
  must fall within 1.8 seconds, match spatially and in size, and come from
  distinct captures. Camera result and capture age must be within 1.2 seconds.
- Forward motion brakes on target loss, stale camera/range data, local arrival
  or the end of a bounded step. Stale motor STATUS aborts. The greeting retains
  its 100 mm local braking target, 80 mm hard guard and arrival latch. Range
  bounce up to 120 mm still requires visual confirmation before celebrating.
- Escape scans use three distinct, settled readings per heading and their
  median at the starting heading, +60, -60 and 180 degrees. A single far
  outlier cannot invent an opening. An opening must measure at least 500 mm
  and is rechecked before travel. ToF measures the nearest obstacle, not the
  identity of a camera target. The 550 mm exit target is not proof of crossing
  a physical exit line.
- Display submission waits at most two seconds; acknowledgement waits at most
  1.5 seconds. Full cue playback has its expression duration plus a 500 ms watchdog margin. Failed final scenes
  terminate directly. Turns use a deadline derived from the selected ramp;
  celebration has a 5.5-second outer watchdog.
- Curious eyes and voice open each action. Seeing the person/exit earns one
  happy acknowledgement. Losing a target earns one puzzled reaction. Object
  identification reveals surprise, then happiness. Greeting ends with LOVE;
  escape ends EXCITED. Faces change while stopped; wiggle sounds use a separate short profile driven
  by WIGGLING/CENTERING events. Full scene expressions and their voices finish
  together, returning to neutral; captions retain the eight-second display policy.

Stop, manual takeover, a new wake during an action, edge/safety events and
connection loss keep cancellation priority. Generation checks prevent delayed
commands from restarting motion, sounds or expressions after Stop. No action
starts at boot or reconnect. Voice direction remains part of wake handling;
an action button does not fabricate a microphone bearing.

### Local validation

Desktop tests cover all three action IDs and retired-ID rejection, model/sensor
readiness, all object labels, missed/incoherent/old/duplicate evidence, 700 ms
camera latency, stopped motor acknowledgements, stuck display/turn/audio waits,
scan outliers, approach arrival, bounded exploration and Stop at 360 mission
times. Standalone camera tracking retains its own regression tests. Voice,
speech, UART, audio, dashboard and responsive layout checks also pass.
The firmware build and model packing pass. Physical approach, object/voice
recognition accuracy and the exit course remain untested for this revision.

## Install

1. On the Zumo, replace **uart_control.py** and **digit_face_engine.py** from
   `../zumo-controller/`. Keep the current **main.py**, face/display adapters,
   **wake_reaction.py**, **voice_turn.py**, and **digit_config.py** installed.
   `voice_turn.py` must be the current fast-wiggle version. The sensor driver must be
   `/zumo_2040_robot/vl53l4cd.py`; its source is in the device-library folder.
   Restart the Zumo. Startup should report the gyroscope and front distance sensor.
2. Open **digit/xiao-sense**, using **xiao_esp32s3_sense_idf**, and upload through
   the XIAO USB-C port. The configured port is COM7; select the actual XIAO port
   if Windows assigned another one. Use normal PlatformIO **Upload**.
3. The upload now includes the bootloader, partition table, application,
   pedestrian and three-class object models, and MultiNet model. **An application-only flash
   is not sufficient for this upgrade.** The offsets changed to fit the larger MultiNet 6 model and both vision models.
4. Open the dashboard and confirm both boards are connected. With motor power
   off, inspect status only. For user-led physical testing, start with What's this,
   then calibrate on safe floor and try Come say hello and the marked exit
   course individually. Check Stop and target loss during each.

The build packs models automatically with `tools/model_bundle.py`. Layout:

| Data | Flash offset | Reserved bytes |
| --- | --- | --- |
| Application | 0x10000 | 0x36b000 |
| Pedestrian + object models | 0x37b000 | 0xe5000 |
| MultiNet model + command graph | 0x460000 | 0x3a0000 |

From this project folder:

```powershell
& C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 4
# Build and print the complete upload command without using the serial port:
& C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 4 -t check_bundle
```

The fixed English audio cues are bundled WAVs, generated locally using Windows
SAPI by `tools/generate_cues.ps1`. The build stores the nine spoken cues as 64-sample IMA ADPCM blocks, reducing
their flash footprint from 572,414 to 160,928 bytes to fit MultiNet 6. Source WAVs
remain unchanged; playback decodes samples incrementally, without allocating
a full cue buffer, and preserves their 16 kHz rate and sample counts.
They play through the ReSpeaker while capture
continues. Wake inference ignores routine audio to prevent self-triggering;
dashboard Stop works during these clips. Wake, vision, and command inference are
serialized. Audio framing uses separate feed/fetch tasks. During the command
window, vision yields to command recognition. Runtime model allocation, processing rate, and recognition
under motor/speaker noise require device testing.

## Verification and physical acceptance

### Chase this 5 cm refinement, 2026-10-05

Chase this now targets 50 mm from the front ToF, with chasing resuming at
100 mm. Below 300 mm it uses short 2400-speed steps with the selected
acceleration and existing fixed braking. The Zumo brakes locally at 50 mm,
reports arrival, and retains a 30 mm immediate guard while braking. Happy
eyes and the caught-up chirp remain; the 50-to-100 mm waiting band prevents
repeated forward starts and repeated arrival cues as the range fluctuates.
The XIAO requires `CLOSE=2` before allowing this action.

All 97 Zumo tests passed, alongside production XIAO action, speech, UART
and dashboard tests. Checks cover passage through 100/80 mm, local arrival
at 50 mm, range bounce, 100 mm resume, older-controller rejection, sensor
and edge stops, target loss, and cancellation. The complete XIAO bundle
built successfully (3,530,647 / 3,584,000 application bytes).

Installed on both boards. The previous Zumo controller is backed up under
`zumo-controller/backups/2026-10-05-before-chase-5cm/`; its replacement's
SHA-256 matched the workspace, and the complete XIAO upload verified flash
hashes. GET-only checks reported `chase_stop_mm=50`, `chase_resume_mm=100`,
`chase_close_supported=true`, both motors zero and no active action, edge
or fault. The voice and camera were ready. No movement was commanded or
tested; the actual stopping gap remains for user testing.

### Find me close-approach refinement, 2026-10-05

The previous action required 60% person confidence while the dashboard drew
55% boxes, and reset its three-result confirmation on any missed detection.
Those mismatches could explain a visible box without acquisition. Find me now
uses the shared 55% threshold and two-of-three confirmation. The dashboard
hides detections whose result or actual camera capture is older than 1.2 seconds
and reports confirmation, fresh-image waiting, distance waiting and edge locks.

Before this update, read-only live status showed Idle / Dashboard Stop and a
Zumo EDGE lock with zero motor outputs. Such a lock blocks movement independently
of visible detections. Edge protection and manual-first control remain in force.

Desktop checks passed for the 10 cm target, short 2400-speed close steps, local
arrival braking, range bounce, 55% hit/miss/hit acquisition, stale sensors,
emergency distance, edge locks, protocol compatibility and cancellation.
All 95 Zumo tests and the XIAO action/speech/UART and dashboard tests passed.
No physical movement testing is performed by the agent for this update.

Installed on both connected boards, with the prior Zumo controller backed up
under `zumo-controller/backups/2026-10-05-before-close-approach/`. The copied
controller SHA-256 matched; the complete XIAO upload verified all flash hashes.
GET-only post-install checks reported `find_me_stop_mm=100`,
`person_confirm_percent=55`, `close_approach_supported=true`, both motors zero,
no active action, no edge/fault lock, and voice/camera/model readiness.
The actual resting gap and improved physical acquisition await user testing.

### Five-action update installed 2026-10-05

- Zumo: backed up the installed controller, face engine, gyro-turn module and
  startup file under `zumo-controller/backups/2026-10-05-before-five-actions/`;
  copied the new controller and face engine, verified SHA-256 matches, and restarted.
  The existing startup and fast-wiggle files already matched the workspace.
- XIAO: built and uploaded the full `xiao_esp32s3_sense_idf` bundle on COM7.
  Application usage is 3,528,235 / 3,584,000 bytes; combined vision is 937,984
  bytes and speech is 3,778,784 bytes. All flash image hashes verified.
- Desktop: 92 Zumo tests passed, along with production action, speech and UART
  controller tests, 840 mission cancellation points, wake takeover, stale-frame
  and distance handling, lost targets, and all five action outcomes. Dashboard
  logic and 320/390/980/1280-pixel layouts passed.
- Post-flash **GET-only** checks: Zumo connected/ready with no fault, both motor
  outputs zero, no turn or action active, `scene_supported=true`,
  `auto_ramp_supported=true`; camera, both model bundles, audio and MultiNet ready.
  The live dashboard contains Hello and all seven new action buttons (three are
  object choices). Detection remains off at idle.

Battery power was off. No action, motor command, voice prompt or movement test
was initiated on either board during acceptance. Physical approach distance,
target tracking, person detection from Digit's height, course behavior and
spoken recognition of the new phrases remain for user testing. A successful
build, simulation and idle status do not establish those physical outcomes.

### Historical voice diagnostics and earlier investigations

For voice diagnosis, `GET /api/actions` reports `voice_ready`, `listening`,
`voice_result`, `recognized`, `command_audio_ms`, `command_chunks`,
`command_mic_peak`, `command_inference_us`, `candidate_command`, and
`candidate_score`, `command_feed_chunks`, `command_fetch_chunks`, `command_model_peak`,
`command_decoder_text`. `audio_total_feeds` and
`audio_total_fetches` show continuous idle processing; `command_dropped_frames`
is a lifetime count. Other audio/chunk/peak measurements reset for each wake window.
Serial logs include window opening, all candidate scores, decoder text at detection
or timeout, timeout measurements,
and audio-queue overflow. Say **Digit**, wait for his answering voice to finish, then
**Stop** for a stationary test: `recognized` must increase and the dashboard
must show **Heard: Stop**, even if the robot was already stopped. A ready model
and complete audio processing alone do not establish command recognition.

The earlier 2026-10-05 MultiNet 7 device check after the audio cleanup confirmed voice, wake, camera,
and both vision models ready. Idle feed/fetch ran at 31.25 frames/s with no
queue drops. The PSRAM allocation change restored dashboard responses and
left approximately 14.8 KB of internal heap (largest block 12.8 KB), rather
than less than 1 KB. This is measured idle headroom, not acceptance of all
simultaneous motor, speaker, camera, and inference loads.

In that MultiNet 7 check, two user-spoken Stop attempts woke Digit successfully. Each command window
processed 198 frames / 6.35 seconds without queue loss; microphone and model
peaks matched (11444 and 8897). MultiNet returned timeout with partial decoder
text `gSTcST` and `cST`, zero candidates, and zero recognized commands. This
confirms audio reaches the decoder, but spoken Stop still does not work. The
redundancy audit is complete; the acoustic/decoder recognition failure remains
unresolved. No extra confidence cutoff discarded a recognized result.

The subsequent MultiNet 6 full-bundle upload also initialized successfully.
Two spoken Stop attempts each delivered 198 frames, with identical microphone
and model peaks (6316 and 7415), no dropped frames, zero candidates and an empty
decoder string at timeout. Changing model versions did not resolve recognition;
the shared audio content and integration still need investigation.

`GET /api/actions/audio.wav` exports the most recent complete command window
as lossless 16 kHz mono signed 16-bit PCM, copied from the exact frames passed
to `detect()`. `X-Command-Session` identifies the recorded session. This is a
bounded seven-second PSRAM diagnostic, replaced on the next processed window
and lost at reboot. It returns 409 before a complete recording exists. Diagnostic
copying never blocks inference; if copying overlaps a new window and loses a
diagnostic frame, that incomplete window is withheld. No microphone gain,
recognition settings or robot actions are changed by exporting audio.

The decoder-only diagnostic accepts raw 16 kHz mono signed 16-bit little-endian
PCM at `POST /api/actions/audio/test?gain=1` (complete 512-sample frames,
maximum seven seconds). `GET` on the same path reports its result. It refuses
an active command window or another test, runs on the sole decoder task, and
never calls the robot action dispatcher or increments live recognition counts.
Normal wake windows are withheld while the test is busy. Gain 1 preserves
samples; optional gains 2 through 8 apply only to the test. Live settings stay
unchanged. Use `tools/replay_speech.py recording.wav --gain 1` for a WAV file.

The first captured Stop recording had 101376 samples / 6.336 seconds, peak 5255.
The locally installed Windows dictation recognizer independently returned
"Stop" at 1.01 seconds, duration 0.77 seconds, confidence 0.749. ESP-SR 2.0.1
direct replays at gains 1, 2 and 4 all returned empty text and zero candidates
at timeout. Generated Stop moving and Say hello clips also failed. All eight
commands were printed active. These checks isolate a decoder integration
failure beyond the microphone, chime timing and live feed queues; they do not
establish the precise cause by themselves.

With ESP-SR 2.4.6 installed, the exact same captured Stop WAV at gain 1
returned `DETECTED`, command 7, text ` STOP`, score 0.967 after 76 frames
(2.432 seconds of input). A repeat at gain 1 reproduced the same result;
gains 2 and 4 returned command 7 with scores 0.965 and 0.962. The speech model
bundle SHA256 is identical before and after the library update. The dependency
resolution also changed ESP-DSP to the required 1.8.0, ESP-DL from 3.3.12 to
3.3.13, and dl_fft to 0.8.0. These results support the library compatibility
diagnosis; they do not identify which internal library change fixed inference.
The generated-voice comparison did not validate the broader vocabulary: its
Stop moving clip incorrectly matched Say hello with score 0.153. Actual
spoken vocabulary accuracy and false-trigger behavior need separate acceptance.

Two subsequent user-spoken Digit / Stop attempts passed live acceptance:
both returned command 7, text ` STOP`, score 0.971, and incremented live
`recognized` from 0 to 2. The action controller advanced its generation from
1 to 3 and reported `Stopped` both times. The windows processed 76 and 67
frames; microphone/model peaks matched at 6197 and 14109, with zero command
or wake queue drops. Audio, wake, camera and both vision models remained ready,
with about 1.60 MB free PSRAM. Zumo `drive_ready` and range validity were false
during this check, so this validates voice recognition and action cancellation,
not physical motor braking or movement. Other spoken commands remain unaccepted.

Desktop checks exercise the actual action controller with mocked hardware,
including Stop at 160 points in Say hello, retired-ID rejection, and stale-command rejection. The real
Zumo controller tests include missing/stale distance, obstacles, edge latching,
transport timeout, and no restart after Stop. Embedded dashboard tests cover
delayed start/drive requests and the single remaining routine button.

These checks and a firmware build do not establish physical motion, accurate
turns, acoustic recognition, or simultaneous-model memory headroom. After
installing both boards, start with tracks lifted: confirm Stop during Say hello,
Button C, and communication-loss stop. Verify turns and edge-stop behavior on
a clear floor without allowing Digit to drive off a surface. No routine starts
automatically at boot, reconnect, or recalibration.


## Historical Follow me release (ID 16, now retired)

Say "Digit", then "Follow me", or press Follow me in the dashboard. The action
starts person detection and turns in place to keep the largest person box
within 8% of the image width from center. Only detections at 55% confidence or
higher are used. The mirrored camera steering matches the established controller.
Tracking uses TURN commands at 3000 with Zumo's 6000 units/s acceleration.
Each correction is 550–700 ms, proportional to the error beyond the center
band. Its deadline is fixed: new detections may stop it early if the person
is centered, missing or across center, but cannot extend it or immediately
reverse it. After each correction, a newer STATUS must show zero tracks;
a 120 ms settling interval follows. Only images exposed after this interval
can start another correction. A missing stopped STATUS cancels after 2 seconds.
The TURN lease is refreshed every 80 ms during a correction;
it never requests forward motion, reverse motion, a contact stroke
or gyro LOOK calibration. Results older than 1200 ms or captures older than
2400 ms stop turning. Lost or centered targets stop turning while leaving the
action armed. Fresh detections can resume tracking until Stop, wake, manual
control or another action cancels it. Link loss, edge, and Zumo faults cancel.

The action owns its tracking heartbeat, so voice-started Follow me works without
an open dashboard. The separate Turn with person camera control retains its
dashboard heartbeat timeout. Follow me needs the person detector and calibrated
Zumo, but not the object model, front ToF, gyro or greeting capabilities.

Bump me, its voice phrase, mission, UART protocol and accelerometer controller
have been removed. ID 15 is retired and rejected; ID 16 prevents stale Bump me
requests from starting the replacement action. The dashboard reports
`follow_action_ready`. Desktop tests cover both steering directions, centered
and lost targets, reacquisition, stale frames, missing optional sensors,
cancellation and old-ID rejection. Physical centering still needs observation.

Installed 2026-10-07: the XIAO `xiao_esp32s3_sense_idf` full bundle on COM7
verified all five flash hashes (application 3,532,943 / 3,584,000 bytes).
Zumo COM6 received `uart_control.py` and `main.py`, both verified by SHA-256
read-back; `attention_bump.py` was removed after backing up all three files to
`zumo-controller/backups/2026-10-07-before-follow-me/`. The earlier relaxed gyro
reference update remains installed. Desktop action/mission/speech/UART and
dashboard tests passed, along with 105 Zumo tests. After restart, GET-only
checks reported `follow_action_ready=true`, `voice_ready=true`, Zumo ready,
no faults and both motors zero. The served dashboard includes Follow me and
no Bump me. No physical tracking or spoken-command acceptance test was run.

Overshoot refinement installed 2026-10-07: XIAO-only upload of the bounded
550–700 ms corrections, 8% center tolerance and post-stop capture gate described
above. Speed 3000 and installed Zumo TURN acceleration 6000 remain in effect.
Action/mission/speech/UART desktop tests passed, including simulated 689 ms
camera latency, repeated frames that cannot extend a correction, and stale
motor status that cannot confirm a stop. The build used 3,533,331 / 3,584,000
application bytes; all five upload hashes verified. GET-only checks confirmed
idle state, voice ready and zero motors. The pre-existing Zumo EDGE lock was
preserved; physical tracking has not been tested with this refinement.
