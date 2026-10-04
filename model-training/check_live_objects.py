"""Exercise the on-device detector through its dashboard API; no drive requests."""
from pathlib import Path
import argparse, json, time, urllib.request
from collections import Counter

HERE = Path(__file__).resolve().parent
CLASSES = ['Battery','Marker','Screwdriver']

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--url',default='http://10.0.0.127')
    parser.add_argument('--stop',action='store_true')
    args=parser.parse_args()
    folder=HERE/'live-check-v3';folder.mkdir(exist_ok=True)
    def request(path,post=False,binary=False):
        req=urllib.request.Request(args.url+path,data=b'' if post else None,method='POST' if post else 'GET')
        with urllib.request.urlopen(req,timeout=10) as response: data=response.read()
        return data if binary else json.loads(data)
    if args.stop:
        reply=request('/api/zumo/stop',True)
        status=request('/api/status')
        assert not status['vision_enabled'] and not status['boxes']
        assert not request('/api/actions')['active']
        (folder/'stop-check.json').write_text(json.dumps(dict(reply=reply,status=status),indent=2))
        print('PASS: global Stop disables object detection and clears boxes')
        return
    startup=request('/api/status')
    assert startup.get('object_classes')==CLASSES,'Device is not running the three-class firmware'
    assert startup['camera_ready'] and startup['battery_ready'], startup
    request('/api/zumo/stop',True)
    generation=request('/api/actions')['generation']
    reply=request(f'/api/vision/objects?enabled=1&generation={generation}',True)
    samples=[]
    deadline=time.monotonic()+20
    while time.monotonic()<deadline and len(samples)<12:
        status=request('/api/status')
        if status['vision_error']: raise RuntimeError(status['vision_error'])
        assert status['battery_mode'] and status['vision_enabled'] and not status['follow_enabled']
        if status['vision_age_ms']<1500:
            for box in status['boxes']:
                assert box['label']==CLASSES[box['class_id']]
            samples.append(status)
            if len(samples) in [1,6,12]:
                (folder/f'live-{len(samples):02d}.jpg').write_bytes(request('/api/camera/photo',binary=True))
        time.sleep(.5)
    assert samples,'No fresh hardware inference results'
    observed=Counter(box['label'] for sample in samples for box in sample['boxes'])
    summary=dict(url=args.url,start_reply=reply,sample_count=len(samples),observed=dict(observed),
                 inference_ms=[s['vision_inference_ms'] for s in samples],
                 interval_ms=[s['vision_interval_ms'] for s in samples],
                 minimum_free_psram=min(s['psram_free'] for s in samples),samples=samples)
    (folder/'live-results.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps({k:v for k,v in summary.items() if k!='samples'},indent=2))
    print('Latest boxes:',json.dumps(samples[-1]['boxes']))
    # Detection is intentionally left on so the user can see it on the dashboard.

if __name__=='__main__': main()
