# Digit XIAO firmware

This is the active XIAO ESP32-S3 Sense application. Open this directory in VS Code
and select **`xiao_esp32s3_sense_idf`**. The project uses ESP-IDF 5.5 through
PlatformIO, pinned to `espressif32 @ 6.12.0`.

For the complete hardware list, wiring, first installation, calibration, and
troubleshooting, start with the [project README](../README.md).

## Current behavior

The XIAO connects directly over Wi-Fi to
`gemini-robotics-er-2-streaming-preview` using a certificate-verified persistent
WebSocket. Gemini accepts spoken requests, typed messages, and camera images.
Replies are brief dashboard text and robot emotes; speech decoding/playback and
TTS are excluded. Local **Hey Digit** and independent **Stop** remain on-device.

An active session sends the newest JPEG at **1 FPS**, including during reasoning,
pending tools, and movement. The shared 1000 ms upload interval also covers
`observe`, post-motion images, and reconnects. Congestion skips uploads instead
of queuing old frames. Uploaded images alone do not initiate a model turn;
idle-only observation prompts supply sensor context without granting motion.
Standby, End, Stop, and connection-only probes send no camera/microphone input.

A fresh user prompt opens a bounded task; later prompts steer it without
resetting limits. Before moving, Gemini must receive a matched `observe` result.
Movement results follow confirmed rest and a newly captured image. Tool results,
model replies, and background prompts cannot open tasks. `finish_task` ends the
task while the observation session remains active; End/Stop closes the session.
There is no silence timeout. The saved timeout limits model response waiting.

Starting limits are 90° per turn, 30 cm forward, 10 cm backward, 50 moves, and
90 seconds per task. Zumo owns sensor feedback, braking, watchdogs, and recovery.
The dedicated 1–30 cm forward push exempts only its close-obstacle threshold.
Measured encoder distance calibration is required for travel. Reconnects never
replay interrupted motor commands or renew task limits.

The dashboard includes manual drive, vision overlays, face previews, speaker
volume, typed chat, task status, Action Console, and the exact last-uploaded JPEG.
**STOP ALL** globally cancels motion, sounds, uploads, previews, and stale pending
work, then opens a fresh idle Gemini connection without conversation resumption.

## Build and upload

1. Copy `src/wifi_credentials.example.h` to ignored `src/wifi_credentials.h` and
   enter your local Wi-Fi credentials.
2. Build the `xiao_esp32s3_sense_idf` environment. PlatformIO/IDF obtain dependencies
   and automatically pack the pedestrian and three-class object models.
3. Run `check_bundle` to inspect the complete upload command without touching hardware.
4. Connect the XIAO USB port, select your actual upload/monitor port, and upload
   through this environment. COM7 is the checked-in example, not a universal port.
5. Read the dashboard address from the 115200-baud serial log.
6. Install Zumo Python separately; XIAO uploads never update Zumo files.

From the repository root:

```sh
pio run -d xiao-sense -e xiao_esp32s3_sense_idf
pio run -d xiao-sense -e xiao_esp32s3_sense_idf -t check_bundle
pio run -d xiao-sense -e xiao_esp32s3_sense_idf -t upload --upload-port COM7
```

Use PlatformIO's VS Code terminal if `pio` is not on PATH. The board has 8 MB flash
and 8 MB PSRAM. `partitions.csv` reserves `0x36b000` bytes for the application,
`0xe5000` at `0x37b000` for the vision bundle, and `gemini_data` at `0x460000` for
two alternating instruction slots. Normal uploads preserve NVS/instructions;
whole-flash erasure removes them. Hey Digit and Stop models are embedded;
MultiNet is not built.

## Camera, audio, and UART

Camera capture is 640 × 480 JPEG with the existing 180° rotation. Dashboard video
is independent of Gemini's 1 FPS upload. Person/object overlays are optional,
off at boot, and do not start movement. The current object classes are Battery,
Marker, and Screwdriver. JPEG capture, streaming, copies, uploads, and preview
share a 64 KiB limit in `src/camera_frame_limits.h`. ESP-DL needs both RGB565→RGB888
and RGB888→RGB888 conversions for the current detector paths.

reSpeaker uses geometry-matched 16 kHz I2S firmware. The processed left channel
feeds Gemini; local Stop receives every captured frame, including during emotion
sounds. Local cues and their acoustic tails are excluded from cloud input.
Read-only routing diagnostics are at `/api/audio/processing`.

Zumo UART is **XIAO D2/GPIO3 TX → Zumo GP29 RX** and
**Zumo GP28 TX → XIAO D3/GPIO4 RX**, with common ground, 115200 baud, 8N1.

## Gemini configuration

Enter a key in **Session setup** on a trusted local network. It is stored in NVS
and never returned by status/settings routes. A blank key field retains the key.
The three editors are **Personality & behavior**, **Base information**, and
**Expression & emotions**. Their document has no fixed 4 KiB editor cap; device
storage, available memory, and model context still bound its practical size.
Save verifies persistence/read-back. Restore buttons affect drafts until Save.

Use **Test connection** first: no media or physical output. **Test-only stream**
allows audio/images and model decisions while physical dispatch remains disabled.
**Start session** and waking Digit use the saved preview setting; typed chat
started from idle leaves the microphone off. Enable robot actions only after
supervised hardware acceptance and distance calibration.

## Development checks

Focused scripts under `tests/` cover transport/tools (`run_gemini_live.ps1`),
sessions (`run_robot_session.ps1`), action/UART control (`run_actions.ps1`),
settings (`run_gemini.ps1`, `run_gemini_config.ps1`), and local Stop
(`run_local_stop.ps1`). They require Visual Studio C++ build tools and the
PlatformIO ESP-IDF packages. Camera/memory/gesture suites remain available.
Browser checks are `test_dashboard.cjs`, `test_gemini_chat.cjs`, and
`test_gemini_instructions.cjs`; they require Playwright and Edge.
`node tools/preview_dashboard.cjs` generates a device-free dashboard preview.

Keep host tests, build, flash, Zumo installation, idle telemetry, cloud checks,
and physical acceptance separate. Desktop/build evidence cannot establish
acoustic wake/Stop accuracy, visible faces, audible cues, or motor behavior.

Further details: [current ER 2 contract](../docs/GEMINI_ROBOTICS_ER2.md),
[Gemini setup](GEMINI_LIVE.md), [voice and Stop](VOICE_ACTIONS.md),
[wire protocol](../docs/TWO_BRAIN_PROTOCOL.md), and
[physical acceptance](../docs/TWO_BRAIN_ACCEPTANCE.md).
