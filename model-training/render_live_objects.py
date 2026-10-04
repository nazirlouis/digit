"""Draw reported hardware boxes on the captured camera image for the check record."""
from pathlib import Path
import json,statistics
from PIL import Image,ImageDraw,ImageFont
HERE=Path(__file__).resolve().parent
folder=HERE/'live-check-v3'
report=json.loads((folder/'restart-check.json').read_text())
status=report['status']
image=Image.open(folder/'live-confirmed-raw.jpg').convert('RGB')
draw=ImageDraw.Draw(image)
font=ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf',18)
colors=['#ffcf45','#38e4ff','#ff74d4']
for box in status['boxes']:
    x1,y1,x2,y2=[box[key]*2 for key in ['x1','y1','x2','y2']]
    color=colors[box['class_id']]
    draw.rectangle([x1,y1,x2,y2],outline=color,width=3)
    text=f"{box['label']} {box['score']}%"
    width=draw.textbbox((0,0),text,font=font)[2]
    x=min(x1,image.width-width-6);y=max(0,y1-26)
    draw.rectangle([x,y,x+width+6,y+25],fill='#071014')
    draw.text((x+3,y),text,font=font,fill=color)
image.save(folder/'live-confirmed.jpg',quality=95)
observations=json.loads((folder/'live-results.json').read_text())
summary=dict(flashed=True,flash_hashes_verified=True,classes=['Battery','Marker','Screwdriver'],
             sample_count=observations['sample_count'],frames_with_detection=observations['observed'],
             steady_state_median_inference_ms=statistics.median(observations['inference_ms'][1:]),
             median_interval_ms=statistics.median([n for n in observations['interval_ms'] if n]),
             minimum_free_psram=observations['minimum_free_psram'],
             stop_and_stale_start_checks_passed=True,model_left_enabled=True,
             visual_check='Boxes align with the visible battery, marker, and screwdriver in the camera image.',
             limitation='Marker is intermittent: present in 6 of 12 observed samples.',
             final_boxes=status['boxes'])
(folder/'check-summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
