# Digit Sense camera and ReSpeaker recorder

This firmware connects the XIAO ESP32S3 Sense to an existing 2.4 GHz Wi-Fi
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
the controls on port 80.

## HTTP interface

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Camera and audio control page |
| GET | `http://DEVICE_IP:81/stream` | MJPEG camera stream |
| GET | `/api/status` | Camera, audio, PSRAM, and Wi-Fi status |
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
