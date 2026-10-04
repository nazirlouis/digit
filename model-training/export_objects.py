"""Export Roboflow v3 to the XIAO's ESP-DL INT8 format and verify all classes."""
from pathlib import Path
import os, sys, json, hashlib, shutil
HERE = Path(__file__).resolve().parent
os.environ['YOLO_CONFIG_DIR'] = str(HERE/'ultralytics-settings')
os.environ['MPLCONFIGDIR'] = str(HERE/'matplotlib-cache')
os.environ['YOLO_OFFLINE'] = 'true'
sys.path.insert(0,str(HERE/'esp-detection'))

def main():
    import torch, onnx, onnxsim
    from ultralytics import YOLO
    import ultralytics.nn.tasks as tasks
    from nn.modules import ESPDetect
    from nn.esp_tasks import custom_parse_model
    from deploy.quantize import CaliDataset
    from esp_ppq import QuantizationSettingFactory, TargetPlatform
    from esp_ppq.api import espdl_quantize_onnx, export_ppq_graph
    from esp_ppq.executor import TorchExecutor
    from object_evaluation import CLASSES, OUTPUTS, evaluate
    tasks.parse_model = custom_parse_model
    torch.set_num_threads(8)
    artifacts = HERE/'artifacts/objects-v3';artifacts.mkdir(parents=True,exist_ok=True)
    checkpoint = HERE/'runs/objects-v3/weights/best.pt'
    shutil.copy2(checkpoint,artifacts/'objects.pt')
    trained = YOLO(str(checkpoint))
    assert list(trained.names.values()) == CLASSES
    model = trained.model.float().cpu().eval().fuse()
    for layer in model.modules():
        if isinstance(layer,ESPDetect): layer.forward=ESPDetect.export_onnx_forward.__get__(layer)
    onnx_path = artifacts/'objects.onnx'
    torch.onnx.export(model,torch.zeros(1,3,224,224),str(onnx_path),input_names=['images'],output_names=OUTPUTS,
                      opset_version=13,do_constant_folding=True,dynamo=False)
    simplified,valid = onnxsim.simplify(onnx.load(str(onnx_path)));assert valid
    onnx.checker.check_model(simplified);onnx.save(onnx.shape_inference.infer_shapes(simplified),str(onnx_path))
    destination = HERE.parent/'xiao-sense/models/objects';destination.mkdir(exist_ok=True)
    output = destination/'objects_224.espdl'
    settings = QuantizationSettingFactory.espdl_setting();settings.equalization=False
    calibration = torch.utils.data.DataLoader(CaliDataset(str(HERE.parent/'training_data/train/images'),224),batch_size=8)
    quantized = espdl_quantize_onnx(onnx_import_file=str(onnx_path),espdl_export_file=str(output),
        calib_dataloader=calibration,calib_steps=32,input_shape=[1,3,224,224],target='esp32s3',num_of_bits=8,
        collate_fn=lambda batch:batch.cuda(),setting=settings,device='cuda',error_report=False,
        skip_export=False,export_test_values=False,verbose=0)
    export_ppq_graph(quantized,TargetPlatform.NATIVE,str(artifacts/'objects.native'))
    executor = TorchExecutor(quantized,device='cuda')
    verification = {split:evaluate(executor,split,rgb565=True,preview=HERE/f'objects-v3-{split}-preview.jpg') for split in ['valid','test']}
    (HERE/'objects-v3-quantized-summary.json').write_text(json.dumps(verification,indent=2))
    for split in verification:
        for name,counts in verification[split]['per_class'].items():
            assert counts['true_positives'] > 0, f'INT8 has lost the {name} detections in {split}; do not flash'
    metadata = dict(model='ESPDet-Pico',class_names=CLASSES,target='esp32s3',quantization_bits=8,
                    input_shape=[1,224,224,3],input_normalization='RGB / 255',resize='letterbox',padding_rgb=[0,0,0],
                    output_names=OUTPUTS,confidence_threshold=.35,nms_threshold=.45,equalization=False,
                    checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                    model_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),model_bytes=output.stat().st_size,
                    dataset='Digit Roboflow version 3',calibration_images=len(calibration.dataset))
    (destination/'metadata.json').write_text(json.dumps(metadata,indent=2))
    print(json.dumps(metadata,indent=2))
    print(json.dumps({split:{k:v for k,v in report.items() if k!='predictions'} for split,report in verification.items()},indent=2))

if __name__=='__main__': main()
