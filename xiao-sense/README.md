# Digit Sense XIAO firmware

This is the XIAO ESP32-S3 Sense dashboard built with ESP-IDF 5.5 through
PlatformIO. This is the active XIAO project to open in VS Code. The previous
Arduino application is preserved in `../xiao-sense-arduino-legacy`.

The **Digit actions** panel and MultiNet commands expose **Come say hello**,
**What's this?**, and **Find a way out**, plus Stop. The current release
adds coordinated character reactions and stricter freshness/timeout checks.
See [action behavior and testing](VOICE_ACTIONS.md) for behavior, validation,
and installation status. The light dashboard and 100% startup speaker volume
were installed together on 2026-10-07. Both controllers passed startup and
GET-only idle checks; physical action and speaker/screen timing acceptance remains pending.

The camera stays at 640 × 480 JPEG with the existing 180° rotation, live
stream, and picture download. The on-demand pedestrian detector is Espressif's
`pico_s8_v1` model. It targets two 320 × 240 frames per second while enabled,
accepts boxes at 55% confidence or above, and draws them over the live feed.
The dashboard shows the measured update rate. The model and stream share the
camera, so the measured rate can vary with Wi-Fi and camera load.
The dashboard checks for new boxes every 250 ms so it can show each model
result without waiting for the old one-second status refresh.
Detection starts off. After floor calibration, press **Start person detection**
and then **Turn with person** to let the Zumo rotate toward the largest person
box. The second button is a separate, visible motor enable.

## Object model test

The locally trained ESPDet-Pico object model is included in the upload bundle.
It uses 224 × 224 RGB input with black letterbox padding, INT8 quantization, and
a 35% confidence threshold. Its classes are Battery, Marker, and Screwdriver.
Both vision models are stored in flash; the camera
task loads one at a time when its dashboard button is pressed.

1. Open this **xiao-sense** project and select **xiao_esp32s3_sense_idf** in
   PlatformIO. Check the XIAO USB port (configured as COM7).
2. Use normal PlatformIO **Upload**. This includes the new partition table,
   firmware, both vision models, and speech models. The model offsets changed;
   uploading only firmware.bin will not install this version correctly.
3. Open the dashboard and press **Test object model**. Battery (yellow), Marker
   (cyan), and Screwdriver (pink) boxes show their names, confidence, and inference
   time on the camera view. Use **Stop object test** or the dashboard's global
   **Stop** to end it. Person detection can be
   selected again with its existing button.

Starting the object test cancels existing routines and person turning. Object
boxes do not enable following or movement. Global Stop also invalidates a delayed
object start request. The object test starts off after reboot.

Local GPU training and the final firmware build have passed. INT8 simulation with
RGB565 color loss detected 16/16 test batteries, 12/12 test markers, and 14/17 test
screwdrivers, with no false detections on the 12 empty validation/test images. These are small,
related-image splits for a first test. This version was flashed to the XIAO and
checked against three visible objects: Battery and Screwdriver appeared in 12/12
samples, Marker in 6/12. Median model inference was 305 ms (about 2.3 result
updates per second), and global Stop/restart passed. Marker detection remains
intermittent. See
[training details](../model-training/README.md) and
[prediction preview](../model-training/objects-v3-test-preview.jpg).

The 8 MB flash is nearly full: application partition ~3.58 MB, combined vision
~0.94 MB, speech ~3.78 MB. The build checks each partition's size. Size optimization is
enabled in sdkconfig.defaults.

The target is centered when its box center is within 8% of the image width
from the middle (about 25 pixels in the 320-pixel inference frame). Outside that
band, the XIAO requests a 550–700 ms correction proportional to the centering
error, at motor speed 3000 with acceleration 6000. Incoming detections cannot
extend the deadline; centered, lost or opposite-side detections can stop it early.
After stopping, it requires a new Zumo status showing zero motor output, allows
120 ms to settle, then waits for an image captured after settling. The mode never asks for forward or
reverse travel, and it does not use ToF distance. When no person is detected,
turning stops until a fresh person detection returns. The **Turn with person** control uses a 1.5-second dashboard lease;
closing that dashboard or losing its connection stops that mode. The Zumo still applies its
own 300 ms motor timeout, edge stop, button C stop, and calibration gate.

The model detects people, not identity. If several people appear, the largest
box is the target. The first physical turning test showed that the camera's
displayed left/right directions needed inversion. `kInvertTurnDirection` in
`src/person_follow.cpp` now applies that correction; repeat the direction check
if the camera mount changes.

The dashboard also retains the ReSpeaker recording, playback, speaker test,
volume control, and UART Zumo controls. Hardware validation must cover those
features separately; a successful build only checks compilation. During this
port's initial hardware check, use Zumo PING/STATUS and FACE commands only.

The dashboard's **Digit face & voice** section previews all 13 expressions:
neutral, happy, sad, mad, serious, curious, surprised, sleeping, confused, sleepy,
excited, crying, and love/heart eyes. **Play voice with face** starts checked;
each button sends the face over UART and plays a matching synthesized reaction.
The thirteen full voices span the corresponding face timelines (0.24-5.55 s),
with distinct contours, breathing/pulsing dynamics, pauses and soft fades.
The existing 90 ms UART lead-in remains approximate; physical screen/audio
latency still needs checking. Use the **Speaker volume** slider (startup 100%).

Action scenes now settle to neutral at the end of their finite expression,
while captions stay readable for eight seconds. Sleeping keeps its closed eyes
and gives one 4.05-second settling breath; it never loops the sound. Gesture
chirps use a separate short timing profile with no lead-in because actual motor
events trigger them. Wake uses a matched 1.8-second happy face/voice response,
within the speech prompt watchdog. Stop cancels all audio and pending work.

Run `tests/run_emotions.ps1` to test/export the real synthesized WAVs, then
`../zumo-controller/tools/render_expression_audio.py` with Pillow and FFmpeg to
render paired face/sound review clips. Outputs are under
`test-artifacts/expression-sync/`. A cross-controller timeline test detects
face/voice duration drift. Installed on 2026-10-07: XIAO firmware and bundled
models on COM7, plus hash-verified `digit_face_engine.py` and `wake_reaction.py`
on Zumo COM6. The remaining nine checked runtime files already matched local
source. Neither the new sound nor panel timing has been auditioned on the device.
Uncheck the switch to compare faces without sound. A new preview replaces the
previous voice; **Stop preview / wake up** cancels actions/audio and requests
neutral eyes. **STOP ALL** also cancels audio and pending previews.

Each phrase plays once, lasts 0.27–1.41 seconds including the lead-in, and uses
the same rounded voice with different pitch contours, rhythm, and vibrato.
Sleeping keeps its face until replaced, but its voice never loops. No preview
requests track movement or needs floor calibration. Controls are unavailable
during movement, calibration, person turning, or an active routine. Voice
previews also require audio ready (finish recording/playback first).

Update the Zumo's face engine and UART controller as described in
`../zumo-controller/README.md` if its installed version does not already support
these faces. This preview feature only changes XIAO firmware. The original
`POST /api/zumo/face?value=NAME` remains face-only. Dashboard previews use
`POST /api/zumo/face?value=NAME&voice=1&generation=N` (or `voice=0`); obtain the
current generation from `/api/actions`. Stale preview requests are rejected
after Stop or another control action. `POST /api/zumo/face/stop` stops previews
and requests neutral. Names are uppercase; Dizzy is not supported.

Voice definitions are in `src/emotion_voice.cpp`; no new audio assets or models
are required. Run `tests/run_emotions.ps1` for synthesis, HTTP-handler and
dashboard checks. Hardware listening and face timing still need an on-device
audition; desktop tests do not establish acoustic quality or LCD behavior.
The Zumo RGB controls can set all six LEDs or one LED, choose a color and
brightness, and turn the selection off. They work while driving remains locked.
The dashboard shows when the Zumo acknowledges the LED command.

## Digit wake reaction

The six-command MultiNet integration is described in [VOICE_ACTIONS.md](VOICE_ACTIONS.md),
including installation, the revised model partitions, and dashboard Stop behavior.

Wake listening starts automatically and stays on during camera streaming,
recording and playback. Wake and vision inference share a lock
and buffered audio. Vision briefly yields during the six-second MultiNet
command window. Digit's answering voice and routine cues suppress wake detection to avoid
self-triggering; the dashboard Stop button remains available throughout.

Saying **Digit** queues the 690 ms Curious robot voice through the ReSpeaker speaker and
a `WAKE` command over the existing UART. The Zumo shows a happy expression and
a blue/cyan/purple sweep followed by a soft pulse. After 2.2 seconds it restores
each LED's previous color/brightness and the face returns to idle. Reactions
have a 2.6-second cooldown; microphone capture continues throughout. MultiNet
waits for the answering voice to finish plus a 120 ms echo tail before accepting
command audio. A stuck prompt is cancelled after 2.5 seconds. Manual LED/face controls override the effect. The reaction does
not arm motors or enable following. With voice turning disabled,
it does not move the robot. A busy/moving/calibrating
Zumo skips its reaction while the XIAO can still answer.

The dashboard shows listening/error status, the last detection's
age and score, and the Zumo acknowledgement. **Test wake reaction** (or
`POST /api/wake/test`) tests lights, face, and sound without requiring speech.
It is unavailable during turns and cooldown. `/api/status` includes `wake_*`
diagnostics, including inference time, microphone peak, and dropped audio frames.
The serial console prints `DIGIT HEARD (score=..., inference=... ms)`.

The exact existing `wake_word_digit/digit.tflite` and manifest are embedded in
the application, using ESP-IDF TensorFlow Lite Micro plus the pinned speech
frontend from the earlier listener. I2S runs full duplex at 16 kHz with GPIO43
as input and GPIO44 as output. A single audio task feeds microphone samples to
both recording and a bounded wake queue. The answering voice replaces speaker
playback when a wake is detected; microphone capture continues.
Queued audio is discarded across Stop, command windows, routine speech, and
overflow, then the streaming model and frontend reset. The flash layout now
includes separate pedestrian and MultiNet model partitions.

Install `wake_reaction.py`, `voice_turn.py`, `uart_control.py`, `main.py`, and
`digit_config.py` on the Zumo before trying this version. Keep the RGB library
and face engine installed; see `../zumo-controller/README.md`. Build/upload this ESP-IDF project,
not the archived Arduino project.

Bench acceptance after both boards are updated:

1. Leave driving locked. Confirm model allocation, full-duplex audio, and
   listening in serial/dashboard, then use **Test wake reaction**. Physically
   check all six LEDs, the happy transition, audible robot voice, and restoration.
2. Say Digit repeatedly with pauses, then try other words and background speech.
   Adjust model sensitivity only after measuring misses and false detections.
3. Enable person detection and check wake responsiveness and dropped-frame
   counters. Say Digit: vision must briefly yield while MultiNet listens.
   Repeat while streaming video and check actual recognition performance.
4. Say Digit during recording, replay, and the speaker test. Confirm microphone
   capture continues and the answering voice replaces playback. Verify the voice does not
   trigger repeated reactions on the actual speaker/microphone arrangement.
5. Override the LEDs/face during an effect, disconnect the Zumo, and reconnect.
   Check acknowledgement/error reporting and that manual choices persist.

A successful build or UART acknowledgement does not prove physical audio,
display output, full-duplex signal quality, or wake accuracy on the device.

## Turn once toward the speaker

Voice turning defaults **on**. Front/left direction calibration and an explicit
on/off choice are saved in XIAO nonvolatile storage. With no saved direction
setup, turning waits for calibration; it also requires floor readiness, updated
Zumo turn support, and a fresh stable bearing. Reset direction setup clears the
mounting calibration but keeps the on/off preference. Mount the circular array
rigidly to Digit so it rotates with the chassis. A microphone sitting separately
on a desk cannot supply a robot-relative heading for this implementation.

1. Upload the active XIAO ESP-IDF application and copy the five Zumo files listed
   above. This does not require reinstalling the Zumo's MicroPython firmware.
2. With vision off and Digit stationary, check **Turn toward my
   voice** in the dashboard. Speak from different sides and confirm the displayed
   array angle changes. Unsupported/busy direction commands report an error;
   they must never be interpreted as a valid zero-degree heading.
3. The microphone mount uses a fixed reference: 270 degrees is front,
   0/360 is left, 180 is right, and 90 is behind. No speech calibration is needed.
4. Speak from front, left, right, and behind. Check the displayed relative
   directions before enabling motion. Preserve this orientation when remounting the array.
5. Place Digit on a clear, level floor and calibrate the floor. Voice turning is
   already enabled unless you switched it off; use **Enable voice turning** if needed. Start with a small left or right angle and say “Digit”. Check the
   physical rotation with STOP/Button C available, then test other headings.
6. During a turn, verify STOP, manual driving, disabling voice turning, starting
   vision, and a UART disconnect each stop it. Button C/edge/gyro faults lock
   motion until floor recalibration. Verify no spontaneous retry or turn loop.

At wake detection, the XIAO freezes a recent speech bearing **before** the answering voice.
The Zumo takes stationary gyro-bias samples, turns to that relative heading,
and stops within a 3-degree band for small turns (up to 30 degrees), or 8 degrees
for larger turns. The face, LEDs, and answering voice start after its
completion reply. Unavailable or uncertain direction, missing gyro support,
or an unarmed/busy robot falls back to the ordinary stationary wake reaction.
An interrupted or failed turn does not play a delayed completion reaction.
**Test wake reaction** always tests only the sound, face and lights.

The direction reader polls in a separate low-priority task, sharing an I2C
transaction lock with audio controls; it never reads the microphone stream.
It first tries `AEC_AZIMUTH_VALUES` / `AEC_SPENERGY_VALUES` for the auto-selected
beam, with `DOA_VALUE` as a firmware-compatibility fallback. It requires speech,
three consistent samples, circular averaging, and a bearing no older than
300 ms. Playback, motion, vision, and transport errors invalidate samples.
See [Seeed's I2C command reference](https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/).
No ReSpeaker firmware change or audio-rate change is made. Support and useful
direction readings on the currently installed 16 kHz Flex image still need
physical confirmation; reflections and competing voices can affect accuracy.

Status `0x40` means the ReSpeaker control servicer wants a retry. Each command
now retries up to eight times, with 10 ms pauses, before advancing to the next
command. The I2C lock covers all attempts. This prevents alternating angle and
speech-energy requests from abandoning each other's unfinished response.
Only status-zero, complete replies update payloads; busy/error bytes never
become bearings. The task still runs separately from microphone capture.
If retries are exhausted, the dashboard names the busy read (angle or speech
energy), while serial logs and `/api/voice/status` include its resource, command
and status. This retry fix requires only an XIAO firmware upload. An on-device
setup retest is needed to establish whether it resolves a particular board's
`0x40` response; it does not prove every Flex firmware exposes those commands.

The native regression test `tests/test_xvf_read.cpp` runs the same transaction
helper as the firmware against an asynchronous mock servicer. It reproduces
the old command-pair starvation, tests successful retries, and checks persistent
busy, terminal errors, short reads, and unchanged payloads on failure. From a
Visual Studio Developer PowerShell in `xiao-sense`, compile and run it with
`cl /nologo /std:c++17 /EHsc /W4 /Fe:test-artifacts/test_xvf_read.exe /Fo:test-artifacts/test_xvf_read.obj tests/test_xvf_read.cpp`
then `./test-artifacts/test_xvf_read.exe`.

`GET /api/voice/status` reports raw/relative angle, freshness, command source,
fixed mounting reference, gyro availability, last read resource/command/status and the last turn result. `POST
/api/voice/setup?action=on|off|reset` controls the saved turning preference;
reset keeps the fixed reference and existing on/off choice. Old sampled front/left
settings are ignored. Once enabled, wake listening and voice
turning work without keeping the browser open; browser visibility still
controls the separate turn-only person tracker. The approved voice actions own
their detection modes and cancel through the same dashboard Stop path.
See [current actions](VOICE_ACTIONS.md) for Come say hello, What's this and
Find a way out. Older object-search, Follow me and Chase commands are retired.

## Build

1. Copy `src/wifi_credentials.example.h` to
   `src/wifi_credentials.h` and set the local Wi-Fi details. The credentials
   file is ignored by Git.
2. From this directory, run
   `C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 2`.
3. Models are packed automatically. Use normal PlatformIO Upload to install
   the application and both model partitions together.

The custom partition table reserves `0x36b000` bytes for the app, `0xe5000` at
`0x37b000` for both vision models, and `0x3a0000` at `0x460000` for MultiNet. This upgrade needs
the new partition table and both models, not just firmware.bin.
Update `upload_port` in `platformio.ini` if Windows assigns a port
other than COM7. The board has 8 MB flash and 8 MB PSRAM.

The original 8 MB Arduino flash image was backed up locally before testing
this port in `backups/xiao-arduino-before-idf.bin`. That directory is ignored
by Git because the image contains device configuration.

## Hardware check

- Confirm startup logs report PSRAM, camera ready, model ready, Wi-Fi address,
  and Zumo UART on GPIO4/GPIO3.
- Open the dashboard, check live video and a downloaded picture, and turn
  detection on. Confirm boxes and confidence values update in the API and
  overlay.
- Check ReSpeaker audio with a real recording and speaker playback.
- With the motors stopped, set all RGB LEDs to a low-brightness color, change
  one LED, then turn all LEDs off. Check the visible color and selection on the
  robot; the UART acknowledgement alone does not prove the LEDs lit.
- With both tracks lifted, verify left/right turning direction, a centered
  person, losing the person, dashboard STOP, and button C. Then calibrate on
  safe floor and try low-speed open-space turning. The first physical turning
  test is for the user to run.

## Hello voice and ramped wiggle

**Come say hello** finds and approaches a person before its excited greeting
wiggle. Zumo sends WIGGLE_START and WIGGLE_CENTER events; XIAO times Excited
and Neutral chirps to those phases. The completed gesture is followed by LOVE
eyes and a soft affectionate phrase. The standalone Say hello action is retired.
STOP ALL, manual takeover, connection loss and safety faults cancel the sequence,
including pending sounds and expressions.

The dashboard **Acceleration** slider applies to driving and wake turns.
It sends `POST /api/zumo/acceleration?value=500..6000`; the
selected value lasts until XIAO reboot (default 6000). Each turn snapshots it,
so an in-progress turn is not changed by a later slider update. Turns use the
Zumo's existing ramp and fixed braking rate. Nonzero turn targets are 3000–3200
to address the reported motor dead zone below 2000; actual ramp output still
passes through lower values when starting and stopping. This is an initial
hardware tuning choice, not a measured universal torque threshold.

Hello uses `WIGGLE`: one stationary gyro preparation, targets near 15 degrees
left and right of the original heading, then a return to that heading. Swing
targets are full 6000, with a dedicated 30000 units/second acceleration and
braking profile through the same ramp engine (200 ms from zero to full target).
Reversals pass through zero without an intermediate settling pause. Gyro rate
and actual motor effort predict braking before each endpoint; small final
centering corrections use 3200. It settles only at center and has a 4500 ms
absolute deadline. These gesture settings do not change the driving slider
or ordinary braking. Full target speed is not necessarily reached in each
short swing. Physical amplitude and timing still require a robot test.

Copy both `voice_turn.py` and `uart_control.py` to the Zumo and restart it.
The XIAO requires `LOOKRAMP=1` before wake turns, plus `WIGGLE=2` for synchronized hello,
preventing an older Zumo controller from silently using its old turn speeds. Slow ramps
receive additional bounded startup time; stall checks use actual motor effort.
Desktop tests cover the reported motor dead zone, continuous wiggle and centering,
acceleration settings, face-before-motion ordering, phase-triggered audio,
cancellation, saved direction settings, and command
listening after playback. Physical heading, torque, sound, and display timing
must still be checked on the robot.

### Desk maze exploration, 2026-10-07

Find a way out drives continuously while fresh front distance exceeds 50 mm.
At a wall it compares settled median readings ahead, 90 degrees left and
90 degrees right, chooses the longest opening, and repeats. It never samples
the rear heading; a dead end stops for help. A battery remains the exit target,
with stationary visual confirmation and bounded approach to 100 mm. The
18-segment / 150-second mission limits and edge/Stop/stale-data guards remain.
All active nonzero movement targets are at least 3000; ramps and braking
still pass through lower values. Physical performance needs a supervised test.


## Two-controller motion jobs (MicroPython Zumo)

Hello and Find a way out now send persistent, checksummed motion jobs. A
separate XIAO UART task services the link while the mission decides what to do.
The task cannot keep a stalled mission alive: mission supervision and the
Zumo heartbeat have separate deadlines. Autonomous actions do not require an
open dashboard or a Wi-Fi connection; manual hold-to-drive retains its lease.

The Zumo completes wall approaches and gyro turns locally. Maze decisions use
three fresh settled measurements bound to their job/heading. Small visual
steering corrections update the existing drive job. New movement actions
require MOTION=1 and calibrated encoder feedback; old firmware is not silently
used as an autonomous fallback. Legacy manual driving and the short greeting
wiggle remain supported.

The Actions debug log now includes job state/result, session, desired and actual
encoder speeds, heading, maximum control gap, overruns and display slice timing.
Scene audio waits for the first visible frame and cancels when a different scene
replaces it. See [the wire protocol](../docs/TWO_BRAIN_PROTOCOL.md) and
[physical acceptance](../docs/TWO_BRAIN_ACCEPTANCE.md).
