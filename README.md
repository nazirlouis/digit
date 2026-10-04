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

## Files shared through Git

The root `.gitignore` keeps training datasets (images and labels), camera
captures, dataset review output, prediction images, logs, caches, build output,
local environments, backups, and credentials on the local PC.

Deployable vision models and their metadata in `xiao-sense/models/`, wake-word
models, source code, training scripts, evaluation summaries, firmware assets,
audio cues, and authored robot face previews can be committed and downloaded.
Intermediate training runs and exports remain excluded by
`model-training/.gitignore`. To retrain, supply your own local `training_data/`
dataset; the training photographs are not included in this repository.
