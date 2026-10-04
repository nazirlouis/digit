"""Match ESP-DL's multi-class raw outputs and class-agnostic NMS."""
from pathlib import Path
import json
import torch
import numpy as np
from PIL import Image, ImageDraw
from torchvision.ops import nms, box_iou

HERE = Path(__file__).resolve().parent
CLASSES = ['Battery', 'Marker', 'Screwdriver']
COLORS = ['#ffcf45', '#38e4ff', '#ff74d4']
OUTPUTS = ['box0', 'score0', 'box1', 'score1', 'box2', 'score2']

def decode(outputs, threshold=.35):
    boxes, scores, classes = [], [], []
    device = outputs[0].device
    for stage, stride in enumerate([8,16,32]):
        distances = outputs[stage*2][0].permute(1,2,0).reshape(-1,4)
        confidence = outputs[stage*2+1][0].sigmoid().permute(1,2,0).reshape(-1,3)
        h,w = outputs[stage*2].shape[-2:]
        yy,xx = torch.meshgrid(torch.arange(h,device=device),torch.arange(w,device=device),indexing='ij')
        centers = torch.stack([xx.flatten(),yy.flatten()],1)*stride+stride/2
        candidates = torch.cat([centers-distances[:,:2]*stride,centers+distances[:,2:]*stride],1)
        locations, categories = torch.where(confidence >= threshold)
        boxes.append(candidates[locations]);scores.append(confidence[locations,categories]);classes.append(categories)
    boxes,scores,classes = torch.cat(boxes).trunc(),torch.cat(scores),torch.cat(classes)
    # ESP-DL runs NMS before clipping and returns at most ten results.
    inclusive = boxes.clone();inclusive[:,2:] += 1
    keep = nms(inclusive,scores,.45)[:10]
    return boxes[keep].clamp(0,223), scores[keep], classes[keep]

def evaluate(executor, split, rgb565=False, preview=None):
    photos = sorted((HERE.parent/'training_data'/split/'images').glob('*.jpg'))
    counts = {name:dict(true_positives=0,false_positives=0,false_negatives=0) for name in CLASSES}
    background_count = background_errors = 0
    rows = []
    sheet = Image.new('RGB',(4*248,((len(photos)+3)//4)*248),'#171717') if preview else None
    for index,photo in enumerate(photos):
        image = Image.open(photo).convert('RGB')
        rgb = np.asarray(image).copy()
        if rgb565: rgb &= np.array([248,252,248],dtype=np.uint8)
        inputs = torch.from_numpy(rgb).permute(2,0,1)[None].float().cuda()/255
        with torch.no_grad(): outputs = executor.forward(inputs,output_names=OUTPUTS)
        boxes,scores,classes = decode(outputs)
        truth,truth_classes = [],[]
        for line in (HERE.parent/'training_data'/split/'labels'/(photo.stem+'.txt')).read_text().splitlines():
            if line.strip():
                cls,x,y,w,h = map(float,line.split())
                truth.append([(x-w/2)*224,(y-h/2)*224,(x+w/2)*224,(y+h/2)*224]);truth_classes.append(int(cls))
        matched = set()
        for box,cls in zip(boxes,classes.tolist()):
            eligible = [j for j,c in enumerate(truth_classes) if c==cls and j not in matched]
            overlaps = box_iou(box[None],torch.tensor([truth[j] for j in eligible],device='cuda')) if eligible else None
            if eligible and overlaps.max().item() >= .5:
                counts[CLASSES[cls]]['true_positives'] += 1;matched.add(eligible[overlaps.argmax().item()])
            else: counts[CLASSES[cls]]['false_positives'] += 1
        for j,cls in enumerate(truth_classes):
            if j not in matched: counts[CLASSES[cls]]['false_negatives'] += 1
        if not truth:
            background_count += 1;background_errors += int(len(boxes)>0)
        rows.append(dict(image=photo.name,boxes=boxes.cpu().tolist(),confidence=scores.cpu().tolist(),class_ids=classes.cpu().tolist(),truth_boxes=truth,truth_class_ids=truth_classes))
        if sheet:
            draw = ImageDraw.Draw(image)
            for box,score,cls in zip(boxes.cpu().tolist(),scores.cpu().tolist(),classes.cpu().tolist()):
                draw.rectangle(box,outline=COLORS[cls],width=2)
                draw.text((max(0,box[0]),max(28,box[1]-12)),f'{CLASSES[cls]} {score:.0%}',fill=COLORS[cls])
            x,y = index%4*248,index//4*248
            sheet.paste(image,(x+12,y+20));ImageDraw.Draw(sheet).text((x+4,y+3),str(index+1),fill='white')
    if sheet: sheet.save(preview,quality=95)
    return dict(threshold=.35,iou_threshold=.5,rgb565=rgb565,per_class=counts,background_images=background_count,background_images_with_false_detections=background_errors,predictions=rows)
