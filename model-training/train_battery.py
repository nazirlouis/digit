"""Fine-tune ESPDet-Pico locally; keep Roboflow's dataset and split intact."""
from pathlib import Path
import os
import sys
import json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
VENDOR = HERE / "esp-detection"
os.environ["YOLO_CONFIG_DIR"] = str(HERE / "ultralytics-settings")
os.environ["YOLO_OFFLINE"] = "true"
os.environ["TORCH_HOME"] = str(HERE / "torch-cache")
os.environ["MPLCONFIGDIR"] = str(HERE / "matplotlib-cache")
(HERE / "ultralytics-settings").mkdir(exist_ok=True)
(HERE / "matplotlib-cache").mkdir(exist_ok=True)
sys.path.insert(0, str(VENDOR))


def main():
    import torch
    import yaml
    from ultralytics import YOLO
    from nn.esp_tasks import custom_parse_model
    import ultralytics.nn.tasks as tasks

    tasks.parse_model = custom_parse_model
    assert torch.cuda.is_available(), "GPU training requires a working CUDA installation"
    torch.set_num_threads(8)
    print("GPU:", torch.cuda.get_device_name(0), "Torch:", torch.__version__, flush=True)
    # Resolve paths explicitly instead of relying on Roboflow's ../ fallback.
    source = yaml.safe_load((ROOT / "training_data/data.yaml").read_text())
    class_names = source["names"]
    assert class_names == ["Battery", "Marker", "Screwdriver"]
    dataset = HERE / "objects-v3.yaml"
    dataset.write_text(yaml.safe_dump({
        "path": str(ROOT / "training_data"), "train": "train/images",
        "val": "valid/images", "test": "test/images", "names": dict(enumerate(class_names)),
    }))
    model = YOLO(str(VENDOR / "cfg/models/espdet_pico.yaml"))
    model.load(str(VENDOR / "examples/cat_detection/espdet_pico_224_224_cat.pt"))
    model.train(
        data=str(dataset), imgsz=224, epochs=200, patience=40, batch=16,
        device=0, workers=0, amp=False, optimizer="AdamW", lr0=0.003,
        lrf=0.05, cos_lr=True, warmup_epochs=3, seed=42,
        mosaic=0.0, mixup=0.0, copy_paste=0.0,
        hsv_h=0.0, hsv_s=0.15, hsv_v=0.15, degrees=7.0,
        translate=0.05, scale=0.15, fliplr=0.5, flipud=0.0,
        project=str(HERE / "runs"), name="objects-v3", exist_ok=True,
        plots=True, save=True,
    )
    best = HERE / "runs/objects-v3/weights/best.pt"
    trained = YOLO(str(best))
    validation = trained.val(data=str(dataset), imgsz=224, device=0, workers=0,
                             split="val", project=str(HERE / "runs"), name="objects-v3-validation")
    test = trained.val(data=str(dataset), imgsz=224, device=0, workers=0,
                       split="test", project=str(HERE / "runs"), name="objects-v3-test")
    predictions = trained.predict(source=str(ROOT / "training_data/test/images"),
                                  imgsz=224, device=0, conf=0.35,
                                  save=True, project=str(HERE / "runs"), name="objects-v3-predictions")
    summary = {"gpu": torch.cuda.get_device_name(0), "torch": torch.__version__,
               "best_checkpoint": str(best), "input_size": [224, 224], "classes": class_names,
               "validation": validation.results_dict, "test": test.results_dict,
               "threshold": 0.35, "test_predictions": [
                   {"image": Path(p.path).name,
                    "boxes": p.boxes.xyxy.cpu().tolist(),
                    "confidence": p.boxes.conf.cpu().tolist(),
                    "class_ids": p.boxes.cls.cpu().int().tolist()} for p in predictions]}
    (HERE / "objects-v3-training-summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
