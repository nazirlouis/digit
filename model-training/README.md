# Digit object training

## Current: three-class Roboflow v3

The updated 337-image dataset was trained locally for 200 epochs on the RTX
5060 Ti. `train_battery.py` now reads the class mapping from `training_data/data.yaml`
and trains Battery, Marker, and Screwdriver together in `runs/objects-v3`.
The original v2 checkpoint and battery model remain available separately.

Final INT8 simulation with RGB565 color loss, 35% confidence, class-agnostic
45% NMS, and at least 50% annotation overlap:

| Class | Validation | Test | False positive boxes, validation/test |
| --- | --- | --- | --- |
| Battery | 30/31 | 16/16 | 0/0 |
| Marker | 25/28 | 12/12 | 0/0 |
| Screwdriver | 21/30 | 14/17 | 0/0 |

No detections occurred on the 8 empty validation images or 4 empty test images.
Desktop test mAP50 is 0.958 and mAP50–95 is 0.670. Calibration uses only the 237
training images. Equalization remains disabled. The model is 502,560 bytes.

Current files:

- `runs/objects-v3/weights/best.pt`: best checkpoint.
- `artifacts/objects-v3/objects.pt`, `objects.onnx`, `objects.native`: export inputs
  and the INT8 simulator graph.
- `../xiao-sense/models/objects/objects_224.espdl` and `metadata.json`: XIAO model
  and exact class order/checksums.
- `objects-v3-training-summary.json`, `objects-v3-quantized-summary.json`,
  `objects-v3-bundle-verification.json`: training, per-class quantized evaluation,
  and firmware/model package verification.
- `objects-v3-test-preview.jpg`, `objects-v3-valid-preview.jpg`: predictions.
- `live-check-v3`: actual camera captures and device API check results.

The complete project was flashed to the connected XIAO ESP32-S3 on COM7, with
all image hashes verified. A full pre-upgrade 8 MB flash backup is retained at
`../xiao-sense/backups/before-objects-v3/flash-8mb.bin` (ignored by Git).
On the live camera, Battery and Screwdriver appeared in 12/12 sampled results;
Marker appeared in 6/12. The boxes align with the objects in the captured image.
Median steady-state inference was 305 ms, with a 434 ms median result interval
(about 2.3 updates per second); minimum reported free PSRAM was 2,263,248 bytes.
Global Stop cleared the detections, a stale start was rejected with HTTP 409,
and a fresh start succeeded. Object detection was left enabled for the user.
The marker's intermittent detection remains a limitation of this first
three-class model. See `live-check-v3/check-summary.json` and `live-confirmed.jpg`.

From the Digit project folder, repeat the current pipeline with:

```powershell
& .\model-training\.venv\Scripts\python.exe .\model-training\train_battery.py
& .\model-training\.venv\Scripts\python.exe .\model-training\export_objects.py
& C:\Users\nazir\.platformio\penv\Scripts\pio.exe run -d xiao-sense -e xiao_esp32s3_sense_idf -j 4 -t check_bundle
& .\model-training\.venv\Scripts\python.exe .\model-training\verify_bundle.py
```

The XIAO dashboard uses **Test object model** and displays class names/colors on
each box. `/api/vision/objects` starts/stops the custom model with the current
control generation; `/api/vision/battery` remains an alias for older clients.
`/api/status` adds `object_classes`, and each box has `class_id` and `label`.
The legacy `battery_mode` field identifies the custom object mode; the actual
`battery_detected` and `battery_score_percent` fields refer only to Battery boxes.
Custom object boxes cannot enable person following. Global Stop cancels a pending
start and clears the object results.

## Archived: first battery-only test

Trained locally on the NVIDIA RTX 5060 Ti 16 GB on 2026-10-03. The original
Roboflow `training_data` split is unchanged: 77 training, 21 validation, and 11
test images, one class (`Battery`), 224 × 224 with black letterbox padding.

The model is Espressif **ESPDet-Pico**, fine-tuned from its cat checkpoint so it
can run through ESP-DL on the XIAO ESP32-S3. The YOLO-format dataset is the input
annotation format. Training ran for 200 epochs on CUDA with batch 16, AdamW,
seed 42, and modest brightness/saturation, rotation, translation, scale, and
horizontal-flip augmentation. The exact options are in `train_battery.py`.

## Results

| Check | Battery boxes found | False detections |
| --- | --- | --- |
| Desktop checkpoint, test | 7 / 7 | 0 on 4 empty images |
| Final INT8, validation with RGB565 color loss | 13 / 13 | 0 on 8 empty images |
| Final INT8, test with RGB565 color loss | 6 / 7 | 0 on 4 empty images |

Detection threshold is 35%, NMS overlap threshold is 45%, and a correct box
requires at least 50% overlap with its annotation. The smallest test battery
(approximately 9 × 23 pixels) is missed by the INT8 model. Desktop validation
mAP50 is 0.995 and mAP50–95 is 0.796; desktop test mAP50–95 is 0.692.
These nearby camera frames are a first test, not independent evidence of broad
accuracy. Actual XIAO allocation, speed, live detection, and coordinate alignment
remain to be checked after the user flashes it.

The first quantization with the upstream equalization option lost all detections.
The final conversion disables equalization and calibrates on the 77 training
images. Held-out images are used for evaluation, not calibration. RGB565 color
simulation matches ESP-DL's expansion to RGB; it does not simulate the whole
camera/JPEG/resize pipeline. The firmware uses RGB/255 and black letterboxing.

## Files

- `runs/battery/weights/best.pt`: original best training checkpoint.
- `artifacts/battery.pt`, `battery.onnx`, `battery.native`: checkpoint copy,
  raw six-output ONNX export, and the quantized simulator graph.
- `../xiao-sense/models/battery/battery_224.espdl`: final 499,936-byte ESP32-S3
  INT8 model installed by normal PlatformIO Upload.
- `training-summary.json`, `quantized-test-summary.json`, and
  `camera-model-summary.json`: numerical results and per-image predictions.
- `quantized-test-preview.jpg`: INT8 test prediction contact sheet.

## Re-run on this PC

The isolated `.venv` is already installed and the two upstream repositories are
present locally. From the Digit project folder:

```powershell
& .\model-training\.venv\Scripts\python.exe .\model-training\train_battery.py
& .\model-training\.venv\Scripts\python.exe .\model-training\export_battery.py
& .\model-training\.venv\Scripts\python.exe .\model-training\verify_camera_model.py
```

Re-training overwrites the local training run. After an export, rebuild the
PlatformIO bundle before uploading. The GPU packages are PyTorch 2.7.1+cu128,
torchvision 0.22.1+cu128, Ultralytics 8.4.172, numpy 1.26.4, ONNX 1.17.0,
onnxsim 0.4.36, and esp-ppq 1.3.11, using Python 3.11.7.

Upstream sources used:

- https://github.com/espressif/esp-detection at
  `28de5902a357d99dd9a5db0f52905255641c5a78`
- https://github.com/espressif/esp-ppq at
  `e8881cb9ea5ba18d314fe84c9ca2ab23a27da6dd`

The model, desktop reports, and preview are retained on this PC. Training caches,
downloaded repositories, virtual environment, large intermediate weights, and
logs are ignored by Git.

## Flash and test

Open `../xiao-sense` with PlatformIO environment `xiao_esp32s3_sense_idf`, select
the XIAO port, and use normal **Upload**. This flashes the partition table,
firmware, both vision models, and MultiNet data. Firmware-only flashing is
insufficient because the layout changed. No hardware was flashed during this task.

After reboot, open the dashboard and press **Test battery model**. Check yellow
boxes/confidence on several battery positions and empty scenes. Confirm **Stop
battery test** and global **Stop** remove results. Select the existing person
button to check switching back. This battery test does not request movement.

The final flash layout is application at `0x10000` (size `0x460000`), combined
vision at `0x470000` (size `0xe5000`), and MultiNet at `0x555000` (size `0x2ab000`).
The build's `check_bundle` target verifies sizes and prints the complete upload
command without executing it.
