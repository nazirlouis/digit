# Digit

Open **`xiao-sense`** in VS Code to build or upload the current XIAO ESP32-S3
Sense dashboard. Its PlatformIO environment is `xiao_esp32s3_sense_idf`
(ESP-IDF). The person detector, camera stream, audio controls, and Zumo UART
controller are in this project.

`xiao-sense-arduino-legacy` preserves the earlier Arduino firmware and is not
an active PlatformIO project. `zumo-controller` contains the Zumo 2040
MicroPython code and is managed separately.

The XIAO's USB port must be connected for an upload. The current
`xiao-sense` source uses a 10% person-turn tolerance.
