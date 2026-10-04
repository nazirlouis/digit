"""Confirm restart after Stop, capture an all-class result, and leave testing on."""
from pathlib import Path
import json,time,urllib.request

HERE=Path(__file__).resolve().parent
folder=HERE/'live-check-v3'
def request(path,post=False,binary=False):
    req=urllib.request.Request('http://10.0.0.127'+path,data=b'' if post else None,method='POST' if post else 'GET')
    with urllib.request.urlopen(req,timeout=10) as response:data=response.read()
    return data if binary else json.loads(data)
generation=request('/api/actions')['generation']
reply=request(f'/api/vision/objects?enabled=1&generation={generation}',True)
seen=[]
deadline=time.monotonic()+12
while time.monotonic()<deadline:
    status=request('/api/status')
    assert status['vision_enabled'] and status['battery_mode'] and not status['follow_enabled']
    if status['vision_error']:raise RuntimeError(status['vision_error'])
    if status['vision_age_ms']<1000:
        seen.append(status)
        if {b['label'] for b in status['boxes']}=={'Battery','Marker','Screwdriver'}:break
    time.sleep(.3)
assert seen
status=seen[-1]
(folder/'live-confirmed-raw.jpg').write_bytes(request('/api/camera/photo',binary=True))
(folder/'restart-check.json').write_text(json.dumps(dict(reply=reply,status=status,sample_count=len(seen)),indent=2))
print('Restart passed; model left enabled. Boxes:',json.dumps(status['boxes']))
