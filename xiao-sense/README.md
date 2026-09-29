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

## Build

1. Copy `src/wifi_credentials.example.h` to
   `src/wifi_credentials.h` and set the local Wi-Fi details. The credentials
   file is ignored by Git.
2. From this directory, run
   `C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf`.
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
- With both tracks lifted, verify left/right turning direction, a centered
  person, losing the person, dashboard STOP, and button C. Then calibrate on
  safe floor and try low-speed open-space turning. The first physical turning
  test is for the user to run.
