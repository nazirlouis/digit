# Digit voice commands and dashboard Stop

Say **Digit**, wait for the chime, then speak one command within five seconds.
The existing custom wake model opens English MultiNet 7. Recognition runs on
the XIAO; no network speech service is used. The same six actions have buttons
in the dashboard's **Digit actions** panel.

| Command | Behavior |
| --- | --- |
| Say hello | Spoken greeting, happy eyes, three small gyro-controlled turns, happy finish. |
| Follow me | Detects the largest person box, turns to center it, approaches in short pulses, and holds around 650 mm according to the front distance sensor. A large box also inhibits approach. Stops on missing/stale detections; shows confused eyes and asks where the person went. |
| Do a dance | Tune, alternating wiggles, forward/back shuffle, two 180-degree turns, changing expressions, completion sound. |
| Look around | Turns left 45 degrees, right 90, then left 45, pausing for camera/range readings. Reports a person, obstacle, or clear space. This is a simple scan, not scene understanding. |
| Take my picture | Centers a visible person without approaching, waits for two centered camera results, speaks a countdown, saves a JPEG, and displays it in the dashboard. Cancels after 45 seconds without a suitable subject. |
| Explore | Continuously drives using the front distance sensor; slows near obstacles, stops and turns 90 degrees to seek another direction. Continues until stopped. No room map or localization. |

**Stop moving** and **Stop** are additional voice controls after the wake word.
The drive pad's **STOP** and the new **STOP ALL** button use the same cancellation
path. They cancel all six routines, the old turn-only person tracker, wake turns,
pending speech results, photo countdown/capture publication, and speaker output.
They flush a motor STOP to UART before returning the dashboard response.
Camera streaming and a previously saved photo remain available.

Dashboard requests carry a generation number. A request or recognition result
from before Stop cannot start a later movement. Motor-producing polling and
HTTP control changes share a mutex; camera downloads and neural inference run
outside it. Saved-photo downloads use asynchronous HTTP so they do not occupy
the dashboard control handler.

All six routines start only after the updated Zumo reports successful floor
calibration. Edge detection, Button C, and sensor/gyro faults keep movement
locked until recalibration. Wi-Fi disconnection also cancels the active routine.
The Zumo still requires motor refreshes within 300 ms. Automatic translation
uses `SAFE left right`: valid front distance samples younger than 250 ms are
required, and forward motion stops at 150 mm. Missing/invalid readings never
count as open space. Manual hold-to-drive retains its existing edge-only rule.

Face changes happen during stopped beats, because LCD transfers must not delay
moving-robot safety checks. Reverse dance steps have edge checks but no rear
distance sensing. Gyro turns require the existing working IMU and motor sign.

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
   is not sufficient for this upgrade.** Model offsets changed to fit both vision models.
4. Open the dashboard, confirm both boards are connected, calibrate on safe floor,
   then try each action button before testing the corresponding spoken command.

The build packs models automatically with `tools/model_bundle.py`. Layout:

| Data | Flash offset | Reserved bytes |
| --- | --- | --- |
| Application | 0x10000 | 0x460000 |
| Pedestrian + object models | 0x470000 | 0xe5000 |
| MultiNet model | 0x555000 | 0x2ab000 |

From this project folder:

```powershell
& C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 4
# Build and print the complete upload command without using the serial port:
& C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -e xiao_esp32s3_sense_idf -j 4 -t check_bundle
```

The fixed English audio cues are bundled WAVs, generated locally using Windows
SAPI by `tools/generate_cues.ps1`. They play through the ReSpeaker while capture
continues. Wake inference ignores routine audio to prevent self-triggering;
dashboard Stop works during these clips. Wake, vision, and command inference are
serialized. During the command window, vision yields, so follow movement waits
for fresh frames. Runtime model allocation, processing rate, and recognition
under motor/speaker noise require device testing.

## Verification and physical acceptance

Desktop checks exercise the actual action controller with mocked hardware,
including Stop at 160 points per routine and stale-command rejection. The real
Zumo controller tests include missing/stale distance, obstacles, edge latching,
transport timeout, and no restart after Stop. Embedded dashboard tests cover
delayed start/drive requests and photo presentation.

These checks and a firmware build do not establish physical motion, accurate
turns, acoustic recognition, or simultaneous-model memory headroom. After
installing both boards, start with tracks lifted: confirm Stop during every
routine and countdown, Button C, and communication-loss stop. Then use a clear
floor to check slow exploration obstacle turns and following distance. Observe
the actual edge-stop behavior without allowing Digit to drive off a surface.
No routine starts automatically at boot, reconnect, or recalibration.

Photo storage holds the latest JPEG in RAM; reboot loses it. Following tracks
the largest detected person, not a remembered individual. Translation speeds,
turn direction, and stop distances are conservative initial settings that still
need verification on this robot.
