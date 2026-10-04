"""Export the local checkpoint and quantize specifically for ESP32-S3."""
from pathlib import Path
import os
import sys
import json
import hashlib
import shutil

HERE = Path(__file__).resolve().parent
os.environ["YOLO_CONFIG_DIR"] = str(HERE / "ultralytics-settings")
os.environ["MPLCONFIGDIR"] = str(HERE / "matplotlib-cache")
os.environ["YOLO_OFFLINE"] = "true"
sys.path.insert(0, str(HERE / "esp-detection"))


def main():
    import torch
    import onnx
    import onnxsim
    from ultralytics import YOLO
    from nn.modules import ESPDetect
    from nn.esp_tasks import custom_parse_model
    import ultralytics.nn.tasks as tasks
    from deploy.quantize import CaliDataset
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.api import espdl_quantize_onnx

    torch.set_num_threads(8)
    tasks.parse_model = custom_parse_model
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    best = HERE / "runs/battery/weights/best.pt"
    shutil.copy2(best, artifacts / "battery.pt")
    model = YOLO(str(best)).model.float().cpu().eval().fuse()
    for layer in model.modules():
        if isinstance(layer, ESPDetect):
            layer.forward = ESPDetect.export_onnx_forward.__get__(layer)
    graph = artifacts / "battery.onnx"
    names = ["box0", "score0", "box1", "score1", "box2", "score2"]
    torch.onnx.export(model, torch.zeros(1, 3, 224, 224), str(graph),
                      input_names=["images"], output_names=names, opset_version=13,
                      do_constant_folding=True, dynamo=False)
    simplified, valid = onnxsim.simplify(onnx.load(str(graph)))
    assert valid
    onnx.checker.check_model(simplified)
    onnx.save(onnx.shape_inference.infer_shapes(simplified), str(graph))
    destination = HERE.parent / "xiao-sense/models/battery"
    destination.mkdir(parents=True, exist_ok=True)
    output = destination / "battery_224.espdl"
    settings = QuantizationSettingFactory.espdl_setting()
    # Equalization caused severe accumulated INT8 error on this fine-tuned model.
    # Plain calibration preserves its validation behavior; verify after export.
    settings.equalization = False
    calibration = torch.utils.data.DataLoader(CaliDataset(str(HERE.parent / "training_data/train/images"), 224), batch_size=8)
    quantized = espdl_quantize_onnx(
        onnx_import_file=str(graph), espdl_export_file=str(output),
        calib_dataloader=calibration, calib_steps=32, input_shape=[1, 3, 224, 224],
        target="esp32s3", num_of_bits=8, collate_fn=lambda batch: batch.cuda(),
        setting=settings, device="cuda", error_report=False,
        skip_export=False, export_test_values=False, verbose=0)
    # Verify detection behavior after quantization, including empty scenes.
    from esp_ppq.executor import TorchExecutor
    from esp_ppq.api import export_ppq_graph
    from esp_ppq import TargetPlatform
    from torchvision.ops import nms, box_iou
    from PIL import Image, ImageDraw
    import numpy as np
    executor = TorchExecutor(graph=quantized, device="cuda")
    export_ppq_graph(quantized, TargetPlatform.NATIVE, str(artifacts / "battery.native"))
    predictions = []
    tp = fp = fn = backgrounds = background_false_positives = 0
    test_images = sorted((HERE.parent / "training_data/test/images").glob("*.jpg"))
    sheet = Image.new("RGB", (3 * 248, ((len(test_images) + 2) // 3) * 248), "#171717")
    for index, photo in enumerate(test_images):
        image = Image.open(photo).convert("RGB")
        inputs = torch.from_numpy(np.asarray(image).copy()).permute(2, 0, 1).float().unsqueeze(0).cuda() / 255
        with torch.no_grad():
            outputs = executor.forward(inputs=inputs, output_names=names)
        if index == 0:
            print("Quantized outputs:", [(name, list(t.shape), t.min().item(), t.max().item()) for name, t in zip(names, outputs)], flush=True)
        boxes, scores = [], []
        for stage, stride in enumerate([8, 16, 32]):
            distances = outputs[stage * 2][0].permute(1, 2, 0).reshape(-1, 4)
            confidence = outputs[stage * 2 + 1][0].sigmoid().reshape(-1)
            h, w = outputs[stage * 2].shape[-2:]
            yy, xx = torch.meshgrid(torch.arange(h, device="cuda"), torch.arange(w, device="cuda"), indexing="ij")
            centers = torch.stack([xx.flatten(), yy.flatten()], dim=1) * stride + stride / 2
            stage_boxes = torch.cat([centers - distances[:, :2] * stride,
                                     centers + distances[:, 2:] * stride], dim=1)
            mask = confidence >= 0.35
            boxes.append(stage_boxes[mask]); scores.append(confidence[mask])
        boxes, scores = torch.cat(boxes).clamp(0, 224), torch.cat(scores)
        keep = nms(boxes, scores, 0.45)
        boxes, scores = boxes[keep], scores[keep]
        label = HERE.parent / "training_data/test/labels" / (photo.stem + ".txt")
        truth = []
        for line in label.read_text().splitlines():
            if line.strip():
                _, x, y, w, h = map(float, line.split())
                truth.append([(x-w/2)*224, (y-h/2)*224, (x+w/2)*224, (y+h/2)*224])
        matched = set()
        for box in boxes:
            ious = box_iou(box.unsqueeze(0), torch.tensor(truth, device="cuda")) if truth else None
            if truth and ious.max().item() >= 0.5 and ious.argmax().item() not in matched:
                tp += 1; matched.add(ious.argmax().item())
            else:
                fp += 1
        fn += len(truth) - len(matched)
        if not truth:
            backgrounds += 1; background_false_positives += int(len(boxes) > 0)
        predictions.append({"image": photo.name, "truth_boxes": truth,
                            "boxes": boxes.cpu().tolist(), "confidence": scores.cpu().tolist()})
        draw = ImageDraw.Draw(image)
        for box, score in zip(boxes.cpu().tolist(), scores.cpu().tolist()):
            draw.rectangle(box, outline="#ffcf45", width=2)
            draw.text((box[0], max(28, box[1]-12)), f"Battery {score:.0%}", fill="#ffcf45")
        x, y = index % 3 * 248, index // 3 * 248
        sheet.paste(image, (x+12, y+20))
        ImageDraw.Draw(sheet).text((x+4, y+3), "Battery" if truth else "No battery", fill="white")
    verification = {"threshold": 0.35, "iou_threshold": 0.5, "true_positives": tp,
                    "false_positives": fp, "false_negatives": fn, "background_images": backgrounds,
                    "background_images_with_false_detections": background_false_positives,
                    "predictions": predictions}
    (HERE / "quantized-test-summary.json").write_text(json.dumps(verification, indent=2))
    sheet.save(HERE / "quantized-test-preview.jpg", quality=95)
    assert tp > 0, "Quantized model has lost every test detection; do not flash it"
    metadata = {
        "model": "ESPDet-Pico", "class_names": ["Battery"],
        "target": "esp32s3", "quantization_bits": 8, "input_shape": [1, 224, 224, 3],
        "input_normalization": "RGB / 255", "resize": "letterbox", "padding_rgb": [0, 0, 0],
        "output_names": names, "confidence_threshold": 0.35,
        "checkpoint_sha256": hashlib.sha256(best.read_bytes()).hexdigest(),
        "model_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "model_bytes": output.stat().st_size,
        "dataset": "Digit Roboflow version 2", "calibration": "77 training images",
        "equalization": False,
    }
    (destination / "metadata.json").write_text(json.dumps(metadata, indent=2))
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
