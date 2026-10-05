# Digit voice commands and dashboard Stop

Say **Digit**, wait for his answering voice, then speak one command within six seconds.
The existing custom wake model opens English MultiNet 6. Recognition runs on
the XIAO; no network speech service is used. **Say hello** is the only routine
available by voice or in the dashboard's **Digit actions** panel. The other
routines are removed while actions are redesigned; Stop remains available.

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

After the Curious wake voice finishes and a 120 ms echo tail passes, MultiNet
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
| Say hello | One quick left/right/center wiggle with happy eyes and synchronized chirps. The Happy voice begins with motion; the soft closing chirp plays during the return to center. Full 6000 swing targets use a dedicated fast ramp and gyro centering. Requires Zumo `WIGGLE=2` support. |

**Stop moving** and **Stop** are additional voice controls after the wake word.
The drive pad's **STOP** and the new **STOP ALL** button use the same cancellation
path. They cancel Say hello, the standalone turn-only person tracker, wake turns,
pending speech results, photo countdown/capture publication, and speaker output.
They flush a motor STOP to UART before returning the dashboard response.
Camera streaming and a previously saved photo remain available.

Dashboard requests carry a generation number. A request or recognition result
from before Stop cannot start a later movement. Motor-producing polling and
HTTP control changes share a mutex; camera downloads and neural inference run
outside it. Saved-photo downloads use asynchronous HTTP so they do not occupy
the dashboard control handler.

Say hello starts only after the updated Zumo reports successful floor
calibration. Edge detection, Button C, and sensor/gyro faults keep movement
locked until recalibration. Wi-Fi disconnection also cancels the active routine.
The Zumo still requires motor refreshes within 300 ms. Automatic translation
uses `SAFE left right`: valid front distance samples younger than 250 ms are
required, and forward motion stops at 150 mm. Missing/invalid readings never
count as open space. Manual hold-to-drive retains its existing edge-only rule.

Face changes happen during stopped beats, because LCD transfers must not delay
moving-robot safety checks. Gyro turns require the existing working IMU and motor sign.
Say hello retains its existing timing and turn behavior. IDs 2 through 6 are
rejected by the speech pipeline, action API, and controller; Stop keeps ID 7.
The build and runtime command registries contain three phrases: SAY HELLO,
STOP MOVING, and STOP. This code change has not been flashed.

## Install

1. On the Zumo, use Thonny to replace **main.py** and **uart_control.py** from
   `../zumo-controller/`. Keep the existing face/display files, **wake_reaction.py**,
   **voice_turn.py**, and **digit_config.py** installed. The sensor driver must be
   `/zumo_2040_robot/vl53l4cd.py`; its source is in the device-library folder.
   Restart the Zumo. Startup should report the gyroscope and front distance sensor.
2. Open **digit/xiao-sense**, using **xiao_esp32s3_sense_idf**, and upload through
   the XIAO USB-C port. The configured port is COM7; select the actual XIAO port
   if Windows assigned another one. Use normal PlatformIO **Upload**.
3. The upload now includes the bootloader, partition table, application,
   pedestrian and three-class object models, and MultiNet model. **An application-only flash
   is not sufficient for this upgrade.** The offsets changed to fit the larger MultiNet 6 model and both vision models.
4. Open the dashboard, confirm both boards are connected, calibrate on safe floor,
   then try Say hello and Stop before testing the corresponding spoken commands.

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
