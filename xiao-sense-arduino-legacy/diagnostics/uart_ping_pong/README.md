# XIAO / Zumo UART bench test

Temporary standalone XIAO firmware. Camera, Wi-Fi and audio are not started.
The main xiao-sense project and its existing build are preserved.

Wiring (3.3 V UART, 115200 baud, 8N1):

| XIAO ESP32-S3 | Zumo 2040 |
| --- | --- |
| D2 / GPIO3, TX | GP29, RX |
| D3 / GPIO4, RX | GP28, TX |
| GND | GND |

LCD CS must already be on GP7, with the buzzer jumper disconnected and the
silent buzzer replacement installed.

Build/upload this folder with PlatformIO. Use the XIAO USB port, not the
ReSpeaker XMOS USB port. Run `zumo-controller/uart_ping_pong_test.py` directly
in Thonny on the Zumo. It runs for 30 seconds without commanding movement.
The XIAO sends `PING X n` every two seconds and responds to `PING Z n`.
The Zumo does the reverse. Matching PONG replies prove a round trip.

Expected Zumo output: `PASS: Zumo -> XIAO -> Zumo round trip`, followed by
nonzero `round_trips` and `XIAO_pings` in the final RESULT. A successful flash
or XIAO TX log alone does not establish that the wire link works.

To restore camera/speaker firmware, build/upload the original `xiao-sense`
project using its `xiao_esp32s3_sense` environment. This UART diagnostic does
not flash the XVF3800 on the Flex.
