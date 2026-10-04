"""Compare a saved camera frame with the exact quantized model on the PC."""
from pathlib import Path
import json
import numpy as np
import torch
from PIL import Image, ImageDraw
from esp_ppq.api import load_native_graph
from esp_ppq.executor import TorchExecutor
from object_evaluation import CLASSES, COLORS, OUTPUTS, decode

HERE=Path(__file__).resolve().parent
torch.set_num_threads(8)
photo=HERE/'live-check-v3/before-flash.jpg'
original=Image.open(photo).convert('RGB')
small=original.resize((320,240),Image.Resampling.BILINEAR)
rgb=np.asarray(small).copy();rgb &= np.array([248,252,248],dtype=np.uint8)
content=Image.fromarray(rgb).resize((224,168),Image.Resampling.BILINEAR)
letterbox=Image.new('RGB',(224,224));letterbox.paste(content,(0,28))
inputs=torch.from_numpy(np.asarray(letterbox).copy()).permute(2,0,1)[None].float().cuda()/255
executor=TorchExecutor(load_native_graph(str(HERE/'artifacts/objects-v3/objects.native')),device='cuda')
with torch.no_grad(): outputs=executor.forward(inputs,output_names=OUTPUTS)
boxes,scores,classes=decode(outputs)
draw=ImageDraw.Draw(original)
predictions=[]
for box,score,category in zip(boxes.cpu().tolist(),scores.cpu().tolist(),classes.cpu().tolist()):
    coords=[box[0]/.35,(box[1]-28)/.35,box[2]/.35,(box[3]-28)/.35]
    draw.rectangle(coords,outline=COLORS[category],width=3)
    draw.text((coords[0],max(0,coords[1]-15)),f'{CLASSES[category]} {score:.0%}',fill=COLORS[category])
    predictions.append(dict(label=CLASSES[category],score=score,box=coords))
original.save(HERE/'live-check-v3/desktop-before-flash.jpg',quality=95)
(HERE/'live-check-v3/desktop-before-flash.json').write_text(json.dumps(predictions,indent=2))
print(json.dumps(predictions,indent=2))
