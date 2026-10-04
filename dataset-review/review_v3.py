from pathlib import Path
from collections import Counter, defaultdict
import json, hashlib, math, re
import numpy as np
import yaml
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / 'training_data'
DEST = ROOT / 'dataset-review/v3'
DEST.mkdir(exist_ok=True)
config = yaml.safe_load((DATA / 'data.yaml').read_text())
names = config['names']
colors = ['#ffcf45', '#38e4ff', '#ff74d4']
issues, duplicates, records = [], [], []
hashes = defaultdict(list)
sizes = Counter()
old_captures = set()
for cache in DATA.glob('*/labels.cache'):
    cached = np.load(str(cache), allow_pickle=True).item()
    for label in cached.get('labels', []):
        old_captures.add(Path(label['im_file']).name.split('_jpg.rf.')[0])
splits = {}
for split in ['train', 'valid', 'test']:
    image_dir, label_dir = DATA / split / 'images', DATA / split / 'labels'
    images = sorted(p for p in image_dir.iterdir() if p.suffix.lower() in ['.jpg','.jpeg','.png'])
    labels = list(label_dir.glob('*.txt'))
    counts = Counter(); image_classes = Counter(); backgrounds = 0
    for photo in images:
        item = dict(index=len(records)+1, split=split, image=photo.name, boxes=[])
        item['new_capture'] = photo.name.split('_jpg.rf.')[0] not in old_captures
        try:
            image = Image.open(photo); image.load(); image = image.convert('RGB')
            sizes[str(image.size)] += 1
            hashes[hashlib.sha256(image.tobytes()).hexdigest()].append(f'{split}/{photo.name}')
        except Exception as exc:
            issues.append(dict(image=photo.name, reason=f'Unreadable image: {exc}')); continue
        label = label_dir / (photo.stem+'.txt')
        if not label.exists():
            issues.append(dict(image=photo.name, reason='Missing label')); continue
        for number, line in enumerate(label.read_text().splitlines(),1):
            if not line.strip(): continue
            values = line.split()
            try:
                assert len(values)==5
                cls,x,y,w,h = map(float,values)
                assert all(math.isfinite(v) for v in [cls,x,y,w,h])
                assert cls == int(cls) and 0 <= cls < len(names)
                assert 0 <= x <= 1 and 0 <= y <= 1 and 0 < w <= 1 and 0 < h <= 1
                assert x-w/2 >= -.005 and y-h/2 >= -.005 and x+w/2 <= 1.005 and y+h/2 <= 1.005
            except Exception:
                issues.append(dict(image=photo.name,line=number,reason='Invalid YOLO detection row',text=line));continue
            cls = int(cls)
            item['boxes'].append(dict(class_id=cls,class_name=names[cls],xyxy=[(x-w/2)*224,(y-h/2)*224,(x+w/2)*224,(y+h/2)*224],width_px=w*224,height_px=h*224))
            counts[names[cls]] += 1
        for cls in {b['class_name'] for b in item['boxes']}: image_classes[cls] += 1
        backgrounds += int(not item['boxes'])
        records.append(item)
    image_stems = {p.stem for p in images}
    for label in labels:
        if label.stem not in image_stems: issues.append(dict(label=label.name,reason='Label without image'))
    splits[split] = dict(images=len(images),label_files=len(labels),background_images=backgrounds,class_images=dict(image_classes),class_boxes=dict(counts))
duplicates = [v for v in hashes.values() if len(v)>1]
def sheets(selected, prefix, crops=False):
    per_page = 30
    for start in range(0,len(selected),per_page):
        page = selected[start:start+per_page]
        sheet = Image.new('RGB',(5*248,6*252),'#181818'); draw=ImageDraw.Draw(sheet)
        for cell,item in enumerate(page):
            image = Image.open(DATA/item['split']/'images'/item['image']).convert('RGB')
            if crops:
                box = item['crop']['xyxy']; x1,y1,x2,y2=box
                margin= max(12,(x2-x1)*.35,(y2-y1)*.2)
                bounds=(max(0,int(x1-margin)),max(0,int(y1-margin)),min(224,int(x2+margin)),min(224,int(y2+margin)))
                image=image.crop(bounds)
                scale=min(224/image.width,224/image.height)
                image=image.resize((round(image.width*scale),round(image.height*scale)))
                ImageDraw.Draw(image).rectangle([(x1-bounds[0])*scale,(y1-bounds[1])*scale,(x2-bounds[0])*scale,(y2-bounds[1])*scale],outline=colors[item['crop']['class_id']],width=2)
            else:
                d=ImageDraw.Draw(image)
                for box in item['boxes']:
                    d.rectangle(box['xyxy'],outline=colors[box['class_id']],width=2)
                    d.text((box['xyxy'][0],max(28,box['xyxy'][1]-11)),box['class_name'],fill=colors[box['class_id']])
            x,y=cell%5*248,cell//5*252
            sheet.paste(image,(x+12+(224-image.width)//2,y+25+(224-image.height)//2))
            draw.text((x+5,y+3),f"#{item['index']} {item['split']} {'NEW' if item['new_capture'] else 'OLD'}",fill='white')
        sheet.save(DEST/f'{prefix}-{start//per_page+1}.jpg',quality=95)
new = [r for r in records if r['new_capture']]
sheets(new,'new-images')
sheets([r for r in records if not r['boxes']],'backgrounds')
for cls,name in enumerate(names):
    crops=[dict(**r,crop=box) for r in records for box in r['boxes'] if box['class_id']==cls]
    sheets(crops,f'{name.lower()}-crops',True)
summary=dict(version=config['roboflow']['version'],classes=names,splits=splits,total_images=len(records),
             previous_capture_count=len(old_captures),new_images=len(new),sizes=dict(sizes),issues=issues,exact_pixel_duplicates=duplicates,
             tiny_boxes=[dict(index=r['index'],image=r['image'],**b) for r in records for b in r['boxes'] if min(b['width_px'],b['height_px'])<6])
(DEST/'audit.json').write_text(json.dumps(summary,indent=2))
(DEST/'image-index.json').write_text(json.dumps(records,indent=2))
print(json.dumps({k:v for k,v in summary.items() if k!='tiny_boxes'},indent=2))
print('Tiny boxes (<6px wide or tall):',len(summary['tiny_boxes']))
