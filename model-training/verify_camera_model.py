"""Evaluate the INT8 simulator with RGB565 color loss, without re-quantizing."""
from pathlib import Path
import json
import torch
import numpy as np
from PIL import Image
from torchvision.ops import nms, box_iou
from esp_ppq.api import load_native_graph
from esp_ppq.executor import TorchExecutor

HERE = Path(__file__).resolve().parent

def main():
    torch.set_num_threads(8)
    graph = load_native_graph(str(HERE / 'artifacts/battery.native'))
    executor = TorchExecutor(graph, device='cuda')
    report = {}
    for split in ['valid', 'test']:
        counts = dict(true_positives=0, false_positives=0, false_negatives=0,
                      background_images=0, background_images_with_false_detections=0)
        rows = []
        for photo in sorted((HERE.parent / 'training_data' / split / 'images').glob('*.jpg')):
            rgb = np.asarray(Image.open(photo).convert('RGB')).copy()
            # ESP-DL expands RGB565 using the high 5/6/5 bits, zero filling the low bits.
            rgb &= np.array([248, 252, 248], dtype=np.uint8)
            image = torch.from_numpy(rgb).permute(2, 0, 1)[None].float().cuda() / 255
            with torch.no_grad():
                outputs = executor.forward(image, output_names=['box0','score0','box1','score1','box2','score2'])
            boxes, scores = [], []
            for stage, stride in enumerate([8, 16, 32]):
                distances = outputs[2*stage][0].permute(1, 2, 0).reshape(-1, 4)
                confidence = outputs[2*stage+1][0].sigmoid().reshape(-1)
                h, w = outputs[2*stage].shape[-2:]
                yy, xx = torch.meshgrid(torch.arange(h, device='cuda'), torch.arange(w, device='cuda'), indexing='ij')
                centers = torch.stack([xx.flatten(), yy.flatten()], 1) * stride + stride / 2
                candidates = torch.cat([centers-distances[:,:2]*stride, centers+distances[:,2:]*stride],1)
                mask = confidence >= .35
                boxes.append(candidates[mask]); scores.append(confidence[mask])
            boxes, scores = torch.cat(boxes).clamp(0,224), torch.cat(scores)
            keep = nms(boxes, scores, .45)
            boxes, scores = boxes[keep], scores[keep]
            truth = []
            for line in (HERE.parent / 'training_data' / split / 'labels' / (photo.stem+'.txt')).read_text().splitlines():
                if line.strip():
                    _, x, y, w, h = map(float, line.split())
                    truth.append([(x-w/2)*224,(y-h/2)*224,(x+w/2)*224,(y+h/2)*224])
            matched = set()
            for box in boxes:
                overlaps = box_iou(box[None], torch.tensor(truth, device='cuda')) if truth else None
                if truth and overlaps.max().item() >= .5 and overlaps.argmax().item() not in matched:
                    counts['true_positives'] += 1; matched.add(overlaps.argmax().item())
                else:
                    counts['false_positives'] += 1
            counts['false_negatives'] += len(truth)-len(matched)
            if not truth:
                counts['background_images'] += 1
                counts['background_images_with_false_detections'] += int(len(boxes)>0)
            rows.append(dict(image=photo.name, boxes=boxes.cpu().tolist(), confidence=scores.cpu().tolist()))
        report[split] = dict(**counts, predictions=rows)
    (HERE / 'camera-model-summary.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:{name:value for name,value in v.items() if name!='predictions'} for k,v in report.items()}, indent=2))

if __name__ == '__main__':
    main()
