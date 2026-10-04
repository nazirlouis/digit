"""Check the actual built package against source models and the flash table."""
from pathlib import Path
import hashlib
import json
import struct

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent / 'xiao-sense'
BUILD = PROJECT / '.pio/build/xiao_esp32s3_sense_idf'
model_path = PROJECT / 'models/objects/objects_224.espdl'
metadata_path = model_path.with_name('metadata.json')
metadata = json.loads(metadata_path.read_text())
assert hashlib.sha256(model_path.read_bytes()).hexdigest() == metadata['model_sha256']
assert hashlib.sha256((HERE / 'runs/objects-v3/weights/best.pt').read_bytes()).hexdigest() == metadata['checkpoint_sha256']
assert metadata['class_names'] == ['Battery', 'Marker', 'Screwdriver']
assert metadata['equalization'] is False
header = (PROJECT / 'src/object_model_config.h').read_text()
assert all('"'+name+'"' in header for name in metadata['class_names'])

partitions = {}
last_end = 0
for line in (PROJECT / 'partitions.csv').read_text().splitlines():
    if not line.strip() or line.lstrip().startswith('#'):
        continue
    name, _, _, offset, size = [s.strip() for s in line.split(',')]
    offset, size = int(offset, 0), int(size, 0)
    assert offset >= last_end and offset+size <= 8*1024*1024
    partitions[name] = (offset,size)
    last_end = offset+size
binary_table = (BUILD / 'partitions.bin').read_bytes()
for index, (name, (offset,size)) in enumerate(partitions.items()):
    magic, _, _, actual_offset, actual_size, label, _ = struct.unpack_from('<HBBII16sI', binary_table, index*32)
    assert magic == 0x50aa and actual_offset == offset and actual_size == size
    assert label.rstrip(b'\0').decode() == name

packed = (BUILD / 'vision_models.bin').read_bytes()
assert packed[:4] == b'PDL2'
count = struct.unpack_from('<I',packed,4)[0]
assert count == 2
sources = {
    'objects_224.espdl': model_path,
    'pedestrian_detect_pico_s8_v1.espdl': PROJECT / 'managed_components/espressif__pedestrian_detect/models/s3/pedestrian_detect_pico_s8_v1.espdl',
}
seen = set()
for index in range(count):
    start, name_start, name_size = struct.unpack_from('<III',packed,8+12*index)
    name = packed[name_start:name_start+name_size].decode()
    original = sources[name].read_bytes()
    assert start % 16 == 0 and packed[start:start+len(original)] == original
    seen.add(name)
assert seen == set(sources)
images = {
    'factory': BUILD / 'firmware.bin',
    'pedestrian_det': BUILD / 'vision_models.bin',
    'model': BUILD / 'digit_models/srmodels.bin',
}
report = {}
for name, file in images.items():
    offset, size = partitions[name]
    length = file.stat().st_size
    assert length <= size
    report[name] = dict(offset=hex(offset), bytes=length, reserved_bytes=size,
                        free_bytes=size-length, sha256=hashlib.sha256(file.read_bytes()).hexdigest())
(HERE / 'objects-v3-bundle-verification.json').write_text(json.dumps(report, indent=2))
print('PASS: built partition table, complete model names and bytes, checkpoint hashes, 8 MB layout and all partition capacities')
print(json.dumps(report, indent=2))
