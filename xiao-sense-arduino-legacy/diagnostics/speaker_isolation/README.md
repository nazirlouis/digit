# Flex speaker isolation test

Temporary XIAO firmware for the circular Flex running 16 kHz I2S firmware.
It uses the ESP32 native I2S driver, GPIO8/7 clocks, GPIO44 output, 32-bit
stereo frames, and continuous silence between tones. The XIAO generates
the clocks. Use this mode only after checking that the Flex is not driving
the shared clock pins.

This diagnostic has no Wi-Fi, camera, microphone recording, or AudioTools.
The normal dashboard is unavailable while it is installed. It starts silent.
On the XIAO serial port at 115200 baud, send `t` for one two-second 440 Hz
sine tone or `s` to return to silence. DAC volume is set to the application's
50% setting. Do not infer acoustic success from a successful write.

Open this directory as a PlatformIO project to build and upload the test.
To restore the application, build/upload the parent `xiao-sense` project.
The diagnostic does not modify the Flex XMOS firmware.
