> Current robot sessions use [Gemini Robotics ER 2](../docs/GEMINI_ROBOTICS_ER2.md), spoken and typed commands and continuous 1 FPS camera input. Hey Digit and spoken Stop remain local. Gemini speaks through emotes; its speech output is not built.

# Digit Sense XIAO firmware

This is Digit's active XIAO ESP32-S3 Sense project, built with ESP-IDF 5.5 through
PlatformIO. Open this folder and select **xiao_esp32s3_sense_idf** before uploading.
The previous Arduino application is preserved in `../xiao-sense-arduino-legacy`.

Digit keeps one Gemini Live connection ready over Wi-Fi, without a PC relay.
The local **Hey Digit** wake word opens microphone upload after the visual wake
acknowledgement and any enabled turn toward the speaker. Local **Stop** and the
dashboard **STOP ALL** cancel speech, motion, camera requests and pending tools.
Inactivity ends the conversation while retaining the ready connection.

The embedded wake model is `wake_word_hey_digit/hey_digit.tflite`, imported with
its manifest from the downloaded `hey_digit.zip`. It uses a 0.5 probability
cutoff, five-frame averaging and 10 ms feature steps. The older `wake_word_digit/`
model remains available for rollback and is not included in the active build.

Fresh spoken prompts and sent dashboard messages automatically open a bounded
task. Gemini can request an observation, turn by angle, travel by distance,
choose expressions and finish the task. Follow-up prompts steer the active task
without resetting its limits. A greeting can be answered and finished without
camera or movement. Navigation requires an initial `observe`; subsequent moves
return fresh images and native tool results on the same connection. Model reply
boundaries do not end the task; Gemini calls `finish_task` when done.

Starting limits are 90 degrees per turn, 30 cm forward, 10 cm backward, 50 moves
and 90 seconds per task. Zumo owns motor feedback and local safety; Gemini cannot
send raw motor power or alter safeguards. The dedicated `push_object` tool
accepts 1–30 cm forward at a fixed maximum target/high acceleration; only its
close-obstacle threshold is exempt for that bounded job. Zumo recovers locally,
then supplies outcomes and fresh images for Gemini to replan. Forward/backward tools require a
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

The **System instructions** area has three separate editors: **Personality &
behavior** for reactions and conduct, **Base information** for Digit's body,
camera, functions, sensors and operating rules, and **Voice instructions** for
tone, pace, pronunciation and speaking style. Each has its own restore
button, which changes only that editor until **Save settings** is pressed. All
sections are saved together on the XIAO and reconnect Gemini with uploads off.
The shared limit is 4096 UTF-8 bytes, including the section headings. Instructions
survive restarts; local Stop and motor limits remain enforced by firmware.

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

JPEG capture, dashboard streaming, detector copies, Gemini uploads and the exact
sent-image preview share a 64 KiB frame limit in `src/camera_frame_limits.h`.
The camera driver uses this limit explicitly for each of its two framebuffers.
ESP-DL enables only RGB565-to-RGB888 and RGB888-to-RGB888 conversion; the latter
is also required for object-model letterbox padding. Revisit both the frame limit
and conversion settings if camera resolution or detector preprocessing changes.

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
now holds `gemini_data`: two alternating instruction-document slots with verified
writes and a committed NVS selector. Local Hey Digit/Stop models are embedded;
MultiNet is no longer built or uploaded. Partition offsets and existing NVS
settings are preserved; existing instruction text migrates on the next save.

## Checks

Run the suites relevant to the change: `tests/run_actions.ps1` for action/UART
control, `run_robot_session.ps1` for session lifecycle, `run_gemini_live.ps1`
for transport/tools, `run_gemini.ps1` and `run_gemini_config.ps1` for settings,
and `run_local_stop.ps1` for independent Stop. Dashboard checks are
`test_dashboard.cjs`, `test_gemini_chat.cjs` and `test_gemini_instructions.cjs`;
the chat suite also checks desktop/phone layouts. Run the Zumo Python unittest
suite after controller changes. Keep tests focused on meaningful behavior and
safety failures; cosmetic edits usually need a preview rather than another
narrow test. Shared motion checks run once during discovery.
ReSpeaker processing/routing diagnostics remain read-only at
`/api/audio/processing`. Startup should report PSRAM, camera/models,
Wi-Fi and Zumo UART on GPIO4/GPIO3.

Keep desktop tests, build checks, XIAO flashing, Zumo installation, idle telemetry,
cloud connection/audio checks and physical acceptance separate. Stationary checks
cannot prove travel accuracy, braking distance, audible playback, visible faces
or spoken wake/Stop accuracy. Test those on the actual robot with supervision.
