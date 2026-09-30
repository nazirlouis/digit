# Digit Sense XIAO firmware

This is the XIAO ESP32-S3 Sense dashboard built with ESP-IDF 5.5 through
PlatformIO. This is the active XIAO project to open in VS Code. The previous
Arduino application is preserved in `../xiao-sense-arduino-legacy`.

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

The target is centered when its box center is within 10% of the image width
from the middle (32 pixels in the 320-pixel inference frame). Outside that
band, the XIAO requests a 180 ms turn at motor speed 4000, then stops and
waits for the next inference result. The mode never asks for forward or
reverse travel, and it does not use ToF distance. When no person is detected,
turning stops. The dashboard renews a 1.5-second control lease while visible;
closing it or losing the connection stops turning. The Zumo still applies its
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

The dashboard's Digit face section offers neutral/wake up, happy, sad, mad,
serious, curious, surprised, sleeping, confused, sleepy, excited, crying, and
love/heart eyes. Update the Zumo's face engine and UART controller as described
in `../zumo-controller/README.md` before trying the new expressions. Sleeping
persists until another face is chosen; other reactions return to idle. Controls
are unavailable during movement, calibration, or person turning. `POST
/api/zumo/face?value=NAME` accepts the uppercase expression names, including
`NEUTRAL`, `SLEEPING`, and `LOVE`. Dizzy is no longer supported.
The Zumo RGB controls can set all six LEDs or one LED, choose a color and
brightness, and turn the selection off. They work while driving remains locked.
The dashboard shows when the Zumo acknowledges the LED command.

## Digit wake reaction

Wake listening starts automatically and stays on during camera streaming,
recording, playback, and the wake chime. Enabling **person detection** pauses
wake inference for the entire time vision is enabled. Stopping person detection
automatically resumes listening after a one-second fresh-audio warmup. There is
no wake on/off switch. Vision and wake inference share a lock so a final vision
frame cannot overlap a resumed wake inference.

Saying **Digit** queues a 400 ms rising chime through the ReSpeaker speaker and
a `WAKE` command over the existing UART. The Zumo shows a happy expression and
a blue/cyan/purple sweep followed by a soft pulse. After 2.2 seconds it restores
each LED's previous color/brightness and the face returns to idle. Reactions
have a 2.6-second cooldown; microphone capture and inference continue during
that cooldown. Manual LED/face controls override the effect. The reaction does
not arm motors or enable following. With voice turning disabled (the boot default),
it does not move the robot. A busy/moving/calibrating
Zumo skips its reaction while the XIAO can still chime.

The dashboard shows listening/vision-pause/error status, the last detection's
age and score, and the Zumo acknowledgement. **Test wake reaction** (or
`POST /api/wake/test`) tests lights, face, and sound without requiring speech.
It is unavailable during vision and cooldown. `/api/status` includes `wake_*`
diagnostics, including inference time, microphone peak, and dropped audio frames.
The serial console prints `DIGIT HEARD (score=..., inference=... ms)`.

The exact existing `wake_word_digit/digit.tflite` and manifest are embedded in
the application, using ESP-IDF TensorFlow Lite Micro plus the pinned speech
frontend from the earlier listener. I2S runs full duplex at 16 kHz with GPIO43
as input and GPIO44 as output. A single audio task feeds microphone samples to
both recording and a bounded wake queue; a chime mixes into speaker playback.
Queued audio is discarded across vision pauses/overflow, then the streaming
model and frontend reset. The application and pedestrian model partition
layout remain unchanged.

Install `wake_reaction.py`, `voice_turn.py`, `uart_control.py`, `main.py`, and
`digit_config.py` on the Zumo before trying this version. Keep the RGB library
and face engine installed; see `../zumo-controller/README.md`. Build/upload this ESP-IDF project,
not the archived Arduino project.

Bench acceptance after both boards are updated:

1. Leave driving locked. Confirm model allocation, full-duplex audio, and
   listening in serial/dashboard, then use **Test wake reaction**. Physically
   check all six LEDs, the happy transition, audible chime, and restoration.
2. Say Digit repeatedly with pauses, then try other words and background speech.
   Adjust model sensitivity only after measuring misses and false detections.
3. Enable person detection: wake must show **Paused for vision** and produce no
   speech-triggered reactions. Disable it: listening must resume automatically.
   Repeat while streaming video; camera streaming alone must never pause wake.
4. Say Digit during recording, replay, and the speaker test. Confirm microphone
   capture continues and the chime overlays playback. Verify the chime does not
   trigger repeated reactions on the actual speaker/microphone arrangement.
5. Override the LEDs/face during an effect, disconnect the Zumo, and reconnect.
   Check acknowledgement/error reporting and that manual choices persist.

A successful build or UART acknowledgement does not prove physical audio,
display output, full-duplex signal quality, or wake accuracy on the device.

## Turn once toward the speaker

Voice turning starts **off** after every XIAO restart. Mount the circular array
rigidly to Digit so it rotates with the chassis. A microphone sitting separately
on a desk cannot supply a robot-relative heading for this implementation.

1. Upload the active XIAO ESP-IDF application and copy the five Zumo files listed
   above. This does not require reinstalling the Zumo's MicroPython firmware.
2. With turning off, vision off, and Digit stationary, check **Turn toward my
   voice** in the dashboard. Speak from different sides and confirm the displayed
   array angle changes. Unsupported/busy direction commands report an error;
   they must never be interpreted as a valid zero-degree heading.
3. Repeat “hello” directly in front while pressing **Set front**. Repeat from
   Digit's left side while pressing **Set left**. These actions do not move
   motors. Left must be 45–135 degrees from the captured front. Both readings
   must be fresh and stable. This calibrates mounting offset and angle sign.
4. Speak from front, left, right, and behind. Check the displayed relative
   directions before enabling motion. Redo setup after remounting the array.
5. Place Digit on a clear, level floor, calibrate the floor, then **Enable voice
   turning**. Start with a small left or right angle and say “Digit”. Check the
   physical rotation with STOP/Button C available, then test other headings.
6. During a turn, verify STOP, manual driving, disabling voice turning, starting
   vision, and a UART disconnect each stop it. Button C/edge/gyro faults lock
   motion until floor recalibration. Verify no spontaneous retry or turn loop.

At wake detection, the XIAO freezes a recent speech bearing **before** the chime.
The Zumo takes stationary gyro-bias samples, turns to that relative heading,
and stops within an 8-degree band. The face, LEDs, and chime start after its
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
setup state, gyro availability, last read resource/command/status and the last turn result. `POST
/api/voice/setup?action=front|left|on|off|reset` performs setup and enabling.
Calibration/enable are session-only. Once enabled, wake listening and voice
turning work without keeping the browser open; browser visibility still
controls the separate person-follow mode. Vision pauses wake listening and
cancels an active voice turn. It resumes listening after vision is stopped.

## Build

1. Copy `src/wifi_credentials.example.h` to
   `src/wifi_credentials.h` and set the local Wi-Fi details. The credentials
   file is ignored by Git.
2. From this directory, run
   `C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 2`.
3. Pack the model with
   `managed_components/espressif__esp-dl/fbs_loader/pack_espdl_models.py`,
   taking its input from
   `managed_components/espressif__pedestrian_detect/models/s3/pedestrian_detect_pico_s8_v1.espdl`.

The custom partition table reserves 6 MB for the app and 1 MB at `0x610000`
for the packed pedestrian model. A normal app upload does not populate that
model partition. On a fresh board, flash the bootloader, partition table,
firmware, and packed model together. Subsequent firmware uploads preserve the
model. Update `upload_port` in `platformio.ini` if Windows assigns a port
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
