# Digit Sense XIAO firmware

This is Digit's active XIAO ESP32-S3 Sense project, built with ESP-IDF 5.5 through
PlatformIO. Open this folder and select **xiao_esp32s3_sense_idf** before uploading.
The previous Arduino application is preserved in `../xiao-sense-arduino-legacy`.

Digit keeps one Gemini Live connection ready over Wi-Fi, without a PC relay.
The local **Digit** wake word opens microphone upload after the visual wake
acknowledgement and any enabled turn toward the speaker. Local **Stop** and the
dashboard **STOP ALL** cancel speech, motion, camera requests and pending tools.
Inactivity ends the conversation while retaining the ready connection.

Gemini can start a bounded task, request an observation, turn by angle, travel
by distance, choose silent facial expressions and finish the task. Camera frames
are uploaded only on request, initially for a task and after each completed move.
Native tool results continue the task on the same connection without another
spoken prompt. A simple greeting does not start a navigation task.

Starting limits are 90 degrees per turn, 30 cm forward, 10 cm backward, 12 moves
and 90 seconds per task. Zumo owns motor feedback and local safety; Gemini cannot
send raw motor power or alter safeguards. Forward/backward tools require a
separate measured encoder-to-distance calibration. That powered floor measurement
is the final setup step, and no scale is guessed when it is absent.

The dashboard retains manual arrows, vision buttons beside them, silent face
controls, a speaker volume slider, task status, debug history and Gemini settings.
Fixed routines, **Turn with person**, emotion sounds and audio recording/replay/
speaker tests have been removed. The local wake acknowledgement is visual.

See [robot tools, task continuation and final calibration](../docs/GEMINI_ROBOT_CONTROL.md),
[voice and local Stop](VOICE_ACTIONS.md), [Gemini setup](GEMINI_LIVE.md) and
[the motion wire protocol](../docs/TWO_BRAIN_PROTOCOL.md).

## Gemini setup

Enter the API key through the dashboard's **Gemini Live setup**. It is saved in
XIAO NVS, survives normal app uploads and is never returned to the page. Local
credential files and named NVS dumps are ignored by Git. Configure it on a trusted
local network because the dashboard uses HTTP.

**Test connection** is microphone-free, camera-free and action-free.
**Test-only stream** enables microphone and speaker communication plus requested
observations, while disabling motor and face dispatch. **Begin conversation** and
wake use the saved preview setting. Stop or End returns to standby, with uploads
and output disabled. Reconnection never resumes cancelled tasks.

## Camera and local vision

The shared camera captures 640 x 480 JPEG with the existing 180-degree rotation.
The dashboard live view remains available independently of cloud observations.
Still-photo storage/downloads are retired. Person and known-object detection are
optional local overlay tools, off at startup; neither enables a movement routine.

The pedestrian detector uses Espressif's `pico_s8_v1` model, accepts boxes at 55%
confidence and targets two 320 x 240 detections per second. Use the dashboard's
measured update rate; actual performance varies with camera and Wi-Fi load.
The object detector recognizes Battery, Marker and Screwdriver. The models are
retained in `models/`; camera capture and resized detector input are separate.

## Build and install

1. Copy `src/wifi_credentials.example.h` to ignored `src/wifi_credentials.h`
   and set local Wi-Fi details.
2. Build `xiao_esp32s3_sense_idf` with PlatformIO. Both vision models are packed
   automatically. `check_bundle` prints the exact combined upload command.
3. Use the matching environment's Upload to install bootloader, partition table,
   application and vision bundle. Update `upload_port` if COM7 changes.
4. Install changed Zumo Python files separately, after backing up and verifying
   device contents. XIAO flashing does not update Zumo files.
5. Configure Gemini, run the connection-only test and Test-only conversation,
   then perform supervised powered acceptance and distance calibration last.

The board has 8 MB flash and 8 MB PSRAM. The app partition is `0x36b000` bytes;
vision occupies `0xe5000` at `0x37b000`. The former speech partition at `0x460000`
is reserved and unused. Local Digit/Stop models are embedded; MultiNet is no
longer built or uploaded. This update preserves partition offsets and NVS.

## Checks

Run `tests/run_actions.ps1`, `run_gemini_live.ps1`, `run_gemini_speech.ps1`,
`run_gemini_config.ps1`, `run_local_stop.ps1` and `test_drive_layout.cjs`, plus the
Zumo Python unittest suite. ReSpeaker processing/routing diagnostics remain
read-only at `/api/audio/processing`. Startup should report PSRAM, camera/models,
Wi-Fi and Zumo UART on GPIO4/GPIO3.

Keep desktop tests, build checks, XIAO flashing, Zumo installation, idle telemetry,
cloud connection/audio checks and physical acceptance separate. Stationary checks
cannot prove travel accuracy, braking distance, audible playback, visible faces
or spoken wake/Stop accuracy. Test those on the actual robot with supervision.
