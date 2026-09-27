# Digit Sense XIAO firmware

## Current firmware: camera, audio, and Zumo dashboard

`src/main.cpp` runs the camera/audio dashboard and a two-way UART link to the
Zumo. The page shows the live camera view on port 81 and adds held movement
buttons, floor calibration, Zumo status, and face controls. The Zumo enforces
edge stopping and a 300 ms drive timeout. Connect XIAO D2/GPIO3 TX to Zumo
GP29 RX, XIAO D3/GPIO4 RX to Zumo GP28 TX, and share ground; both sides use
3.3 V UART at 115200 8N1.

## Preserved wake-word listener

`reference/main_wake_word.cpp` continuously reads the XVF3800/ReSpeaker I2S microphone,
processes the audio with the microWakeWord feature generator, and runs the
model in `wake_word_digit/digit.tflite` on the XIAO. Open the XIAO USB serial
port at **115200 baud**. When the model detects the wake word, it prints
`DIGIT HEARD (score=...)`. A brief `Listening: ...` line every five seconds
shows that microphone samples and inference are still moving. That application
does not start the camera or dashboard.

The model settings come from `wake_word_digit/digit.json`: 10 ms feature step,
five-probability sliding average, and 0.5 probability cutoff. The XIAO
automatically uses the ReSpeaker's external I2S clock when present and supplies
its own clock for the standard XVF3800 I2S firmware. Connect through the XIAO
USB-C port for uploads and serial output. A physical spoken-word check is still
needed after flashing a different XIAO or ReSpeaker assembly.

## Camera and recorder application

The earlier camera/audio entry point is preserved in
`reference/main_camera_audio.cpp`. The active `src/main.cpp` adds Zumo UART
control to that application.

That application connects the XIAO ESP32S3 Sense to an existing 2.4 GHz Wi-Fi
network and serves a camera page with a short ReSpeaker audio recorder. It
keeps the newest recording in PSRAM, plays it automatically when recording
stops, and can replay it through the ReSpeaker's JST speaker output.

## Development setup

Use **VS Code with the PlatformIO extension**. The project uses the Arduino
framework, so the source is Arduino C++ even though Arduino IDE is not needed.

1. Open this `xiao-sense` folder in VS Code.
2. Let PlatformIO install the pinned ESP32 platform and AudioTools dependency.
3. Connect the computer to the **XIAO USB-C port**.
4. Use PlatformIO's Build, Upload, and Serial Monitor buttons. The monitor runs
   at 115200 baud.

Wi-Fi credentials are stored in `include/wifi_credentials.h`. That file is
ignored by Git so the password is not committed. Use
`include/wifi_credentials.example.h` as the template if the private file needs
to be recreated.

## Hardware preparation

The ReSpeaker must run the official **16 kHz I2S firmware**, not its USB-audio
firmware. In I2S mode it intentionally stops appearing as a Windows USB audio
device. Follow Seeed's current firmware instructions:

https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/

Use the XMOS/ReSpeaker USB-C port beside the 3.5 mm jack only when changing the
XVF3800 firmware. Use the XIAO USB-C port for this project's upload and serial
monitor.

Before uploading the combined project, run Seeed's standalone I2S
record/playback test:

https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_record_playback/

Also check the following:

- Connect a suitable speaker to the JST 5W speaker connector.
- Remove the microSD card. GPIO7 and GPIO8 are used by both the Sense microSD
  slot and the ReSpeaker I2S bus, so this project never initializes the card.
- Attach the XIAO Wi-Fi antenna before powering the camera stream.
- Face the ReSpeaker microphone openings toward the sound source.

## Using the page

1. Power the assembly and open the serial monitor at 115200 baud.
2. Wait for `Wi-Fi: connected, open http://...`.
3. Make sure the viewing phone or computer is connected to the same Wi-Fi
   network, then open the printed address.
4. Select **Record**, speak, and then select **Stop & play**. Recording also
   stops and plays automatically at ten seconds.
5. Select **Replay** to hear the retained clip again or **Stop playback** to
   interrupt it.

The live MJPEG camera stream is served separately on port 81 so it cannot block
the controls on port 80. The image is rotated 180 degrees in the camera sensor
for both the stream and still pictures. Select **Take picture** to download a
640×480 JPEG to the viewing phone or computer.

Select **Start person detection** to have the XIAO check camera frames with a
small person-presence model. Detection is off at startup. At least 55% person
score on two positive checks shows a **Naz Detected** badge over the live video.
Select **Stop person detection** to stop checking frames and clear the badge;
it also clears after two negative results or if detection becomes stale. This label
means *a person* was detected, not that the person was identified as Naz.
Detection does not drive the Zumo. The bundled model is TensorFlow Lite Micro's
[`person_detect.tflite`](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/models/person_detect.tflite)
(SHA-256 `808cfdfc0cf3a6fa6f6fa26bfa379ea97c16d5db7334637766e39c3408502e9d`);
the first on-device run must confirm model startup, speed, and accuracy with
the camera, audio, and UART active.
The 115200-baud XIAO serial monitor prints preparation time, model time, total
time, both class scores, and whether the badge is on for each checked frame.

For Zumo control, save `zumo-controller/main.py` and
`zumo-controller/uart_control.py` to the Zumo MicroPython device root and reset
it. Place the robot on safe floor and select **Calibrate floor** before holding
a direction. The page displays Zumo connection, readiness, battery voltage,
and fault. Test direction and stopping with the tracks lifted first.

## HTTP interface

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Camera and audio control page |
| GET | `http://DEVICE_IP:81/stream` | MJPEG camera stream |
| GET | `/api/camera/photo` | Capture one JPEG still image |
| GET | `/api/status` | Camera, audio, person detection, PSRAM, and Wi-Fi status |
| POST | `/api/vision?enabled=1` or `enabled=0` | Start or stop person detection |
| GET | `/api/zumo/status` | Zumo connection, readiness, battery, speeds, and fault |
| POST | `/api/zumo/drive?left=4000&right=4000` | Renew held motor speeds; each side is -6000 to 6000 |
| POST | `/api/zumo/stop` | Stop and clear held movement |
| POST | `/api/zumo/calibrate` | Start floor-edge calibration while stopped |
| POST | `/api/zumo/face?value=HAPPY` | Play HAPPY or SAD on the Zumo face |
| POST | `/api/record/start` | Start a new recording |
| POST | `/api/record/stop` | Stop and automatically play it |
| POST | `/api/play` | Replay the newest recording |
| POST | `/api/play/stop` | Stop current playback |

## First physical test

Keep the speaker away from your ears and begin at the configured conservative
volume. Verify the standalone audio example first, then the camera alone, then
the combined page. During the combined test, confirm that video continues while
recording and playback occur. Repeat at least ten record/play cycles while
watching the serial monitor for resets, I2C failures, or PSRAM errors.

The XIAO ESP32S3 Sense can become hot during sustained camera streaming. Use a
stable power source, provide airflow, and stop the test if the board becomes
unusually hot or unstable.
