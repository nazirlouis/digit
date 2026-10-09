"""Preview the production circle face/color choreography without hardware."""
from pathlib import Path
import sys
import time
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "zumo-controller"), str(ROOT / "zumo-controller/tests")]
from circle_show import CircleShow
from digit_face_engine import FaceEngine
from test_face_engine import Display
from test_uart_control import Clock, RGBLEDs

clock = Clock()
time.ticks_ms = clock.ticks_ms
time.ticks_diff = clock.ticks_diff
time.ticks_add = clock.ticks_add
face = FaceEngine(Display())

class PreviewFace:
    cooperative = True
    def show_scene(self, *args, **kwargs): face.show_scene(*args, **kwargs)
    def play(self, *args, **kwargs): face.play(*args, **kwargs)

leds = RGBLEDs()
show = CircleShow(PreviewFace(), leds, clock)
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 20)
small = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 14)
frames = []
for stamp in range(0, 4800, 60):
    clock.now = stamp
    show.update(stamp); face.update()
    color = face.color
    rgb = (((color >> 11) & 31)*255//31, ((color >> 5) & 63)*255//63, (color & 31)*255//31)
    pixels = Image.new("RGB", (136, 112))
    pixels.putdata([rgb if face.display.pixel(x,y) else (0,0,0) for y in range(112) for x in range(136)])
    picture = Image.new("RGB", (440, 448), (16, 20, 27))
    draw = ImageDraw.Draw(picture)
    draw.text((18, 10), "Circle dance", font=font, fill="white")
    picture.paste(pixels.resize((408, 336), Image.Resampling.NEAREST), (16, 44))
    for i in range(6):
        brightness = leds.brightness[i]/12
        color = tuple(int(v * (.35 + .65*brightness)) for v in leds.colors[i])
        x = 50+i*68
        draw.ellipse((x-12,388,x+12,412),fill=color)
    draw.text((18, 425), "Face + light preview · existing dance music repeats", font=small, fill="#b5c6d8")
    frames.append(picture)
out = ROOT / "xiao-sense/test-artifacts/circle-show"
out.mkdir(parents=True, exist_ok=True)
frames[0].save(out / "circle-show.gif", save_all=True, append_images=frames[1:], duration=60, loop=0)
frames[25].save(out / "circle-show.png")
print(out / "circle-show.gif")
