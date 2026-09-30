"""Desktop preview from the same monochrome pixels used by the robot.

Requires Pillow. Run from any directory; writes previews beside the firmware.
This simulates the animation clock, not the physical panel's transfer speed.
"""

import pathlib
import sys
import time
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
from digit_face_engine import FaceEngine, EXPRESSIONS, STYLES
from test_face_engine import Display

now = 0
time.ticks_ms = lambda: now
time.ticks_add = lambda a, b: a + b
time.ticks_diff = lambda a, b: a - b
OUT = ROOT / "previews"
OUT.mkdir(exist_ok=True)
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 18)
small = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 14)


def snapshot(display, color):
    foreground = (((color >> 11) & 31) * 255 // 31,
                  ((color >> 5) & 63) * 255 // 63,
                  (color & 31) * 255 // 31)
    picture = Image.new("RGB", (display.w, display.h))
    picture.putdata([foreground if display.pixel(x, y) else (0, 0, 0)
                     for y in range(display.h) for x in range(display.w)])
    return picture.resize((272, 224), Image.Resampling.NEAREST)


def gallery():
    canvas = Image.new("RGB", (1184, 1120), (16, 18, 22))
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 12), "DIGIT / EXPRESSION GALLERY", font=font, fill="white")
    draw.text((24, 40), "Rendered from the face code. Animation timing still needs an on-robot check.", font=small, fill=(165, 172, 185))
    for i, name in enumerate(EXPRESSIONS):
        face = FaceEngine(Display())
        face.play(name)
        global now
        now += STYLES[name][1] + (0 if name == "excited" else 250)
        face.update()
        x, y = 16 + i % 4 * 296, 78 + i // 4 * 258
        canvas.paste(snapshot(face.display, face.color), (x, y))
        draw.text((x + 8, y + 227), name.capitalize(), font=font, fill="white")
    canvas.save(OUT / "digit-expressions.png")


def comparison():
    """Keep the two easily confused expressions next to one another."""
    global now
    names = ("curious", "confused")
    faces = [FaceEngine(Display()) for _ in names]
    for face, name in zip(faces, names):
        face.play(name)
    frames = []
    for step in range(72):
        frame = Image.new("RGB", (592, 280), (16, 18, 22))
        draw = ImageDraw.Draw(frame)
        for index, face in enumerate(faces):
            face.update()
            x = 12 + index * 296
            frame.paste(snapshot(face.display, face.color), (x, 12))
            draw.text((x + 8, 242), names[index].capitalize(), font=font, fill="white")
        frames.append(frame)
        if step == 10:
            frame.save(OUT / "curious-vs-confused.png")
        now += 60
    frames[0].save(OUT / "curious-vs-confused.gif", save_all=True,
                   append_images=frames[1:], duration=60, loop=0)


def animation(names=("happy", "sad", "crying", "love"), filename="digit-face-motion.gif"):
    global now
    faces = [FaceEngine(Display()) for _ in names]
    for face, name in zip(faces, names):
        face.play(name)
    frames = []
    for _ in range(76):
        frame = Image.new("RGB", (592, 528), (16, 18, 22))
        # Compare four expressions without distorting the physical aspect ratio.
        draw = ImageDraw.Draw(frame)
        for i, face in enumerate(faces):
            face.update()
            thumb = snapshot(face.display, face.color)
            x, y = 12 + (i % 2) * 296, 12 + (i // 2) * 264
            frame.paste(thumb, (x, y))
            draw.text((x + 8, y + 230), names[i].capitalize(), font=font, fill="white")
        frames.append(frame)
        now += 60
    frames[0].save(OUT / filename, save_all=True,
                   append_images=frames[1:], duration=60, loop=0)


if __name__ == "__main__":
    gallery()
    animation()
    animation(("mad", "surprised", "sleeping", "love"), "digit-color-motion.gif")
    comparison()
    print(OUT / "digit-expressions.png")
    print(OUT / "digit-face-motion.gif")
    print(OUT / "digit-color-motion.gif")
    print(OUT / "curious-vs-confused.png")
    print(OUT / "curious-vs-confused.gif")
