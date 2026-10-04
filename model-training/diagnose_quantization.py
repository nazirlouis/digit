from pathlib import Path
import torch
import numpy as np
from PIL import Image
from esp_ppq.api import load_native_graph
from esp_ppq.executor import TorchExecutor
import onnxruntime as ort

here = Path(__file__).resolve().parent
torch.set_num_threads(8)
photo = sorted((here.parent / 'training_data/test/images').glob('*.jpg'))[0]
data = np.asarray(Image.open(photo).convert('RGB')).copy().transpose(2, 0, 1)[None].astype(np.float32) / 255
original = ort.InferenceSession(str(here / 'artifacts/battery.onnx')).run(None, {'images': data})
graph = load_native_graph(str(here / 'artifacts/battery.native'))
executor = TorchExecutor(graph, device='cuda')
def report(label):
    outputs = executor.forward(torch.from_numpy(data).cuda())
    print(label, [(name, round(t.min().item(), 4), round(t.max().item(), 4), round(np.mean((t.detach().cpu().numpy()-ref)**2), 4)) for name,t,ref in zip(graph.outputs,outputs,original)], flush=True)
report('quantized')
for op in graph.operations.values():
    if hasattr(op, 'dequantize'):
        op.dequantize(expire_device='cuda')
executor.load_graph(graph)
report('dequantized')
print('original', [(t.min().item(), t.max().item()) for t in original])
