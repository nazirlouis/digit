"""Desktop-only mouth concepts over Digit's unmodified production eyes.

Uses saved voice samples when available. Never imports hardware or installs code.
Run with Pillow and FFmpeg to recreate the PNG, GIF, and sound-enabled MP4.
"""

from array import array
import math
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time
import wave

from PIL import Image, ImageDraw, ImageFont

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / "tests")]
from digit_face_engine import FaceEngine, EXPRESSIONS, STYLES
from test_face_engine import Display

OUT = ROOT / "previews"
WAVS = ROOT.parent / "xiao-sense/test-artifacts/emotions"
FPS, SR = 20, 16000
now = 0
time.ticks_ms = lambda: now
time.ticks_add = lambda a, b: a + b
time.ticks_diff = lambda a, b: a - b
BG, FG, MUTED = "#11151b", "#edf1f6", "#a7b1c0"
FONT = "C:/Windows/Fonts/segoeui.ttf"
TITLE = ImageFont.truetype(FONT, 26)
LABEL = ImageFont.truetype(FONT, 19)
SMALL = ImageFont.truetype(FONT, 15)
OPTIONS = (("oval", "A / Tiny oval", "A quiet line opens into a little oval"),
           ("smile", "B / Soft smile", "A curved mouth with a wider opening"),
           ("bars", "C / Voice bars", "Five small bars pulse with the sound"))


def rgb(color):
    return ((color >> 11 & 31) * 255 // 31,
            (color >> 5 & 63) * 255 // 63, (color & 31) * 255 // 31)


def eyes(face):
    pic = Image.new("RGB", (136, 112))
    color = rgb(face.color)
    pic.putdata([color if face.display.pixel(x, y) else (0, 0, 0)
                 for y in range(112) for x in range(136)])
    return pic


def mouth_mask(style, level, expression, phase):
    """Only native face-sized primitives; no changes to the eye layout."""
    mask = Image.new("1", (136, 112))
    d = ImageDraw.Draw(mask)
    level = max(0, min(1, level))
    cx, cy = 68, 99
    if style == "oval":
        if level < .10:
            d.rounded_rectangle((cx-6, cy, cx+6, cy+1), radius=1, fill=1)
        else:
            w, h = round(11+5*level), round(3+9*level)
            d.ellipse((cx-w//2, cy-h//2, cx+w//2, cy+h//2), fill=1)
            if h >= 6:
                d.ellipse((cx-w//2+2, cy-h//2+2, cx+w//2-2, cy+h//2-2), fill=0)
    elif style == "smile":
        sad = expression in ("sad", "mad", "crying")
        points = []
        for x in range(-9, 10):
            arch = round(3*(1-(x/9)**2))
            points.append((cx+x, cy+(-arch if sad else arch)))
        if level < .10:
            d.line(points, fill=1, width=2)
        else:
            h = round(4+8*level)
            d.rounded_rectangle((cx-9, cy-3, cx+9, cy+h-3), radius=5, fill=1)
            if h > 6:
                d.rounded_rectangle((cx-7, cy-1, cx+7, cy+h-5), radius=3, fill=0)
    else:
        for i, weight in enumerate((.5, .8, 1., .8, .5)):
            wobble = .85+.15*math.sin(phase*19+i*1.3)
            h = 2 if level < .10 else round(2+10*level*weight*wobble)
            x = cx-11+i*5
            d.rounded_rectangle((x, cy-h//2, x+2, cy+(h-1)//2), radius=1, fill=1)
    return mask


def picture(face, style, level, phase):
    pic = eyes(face)
    mask = mouth_mask(style, level, face.expression, phase)
    # Catch any concept that would cover an existing eye pixel.
    assert all(not (mask.getpixel((x, y)) and face.display.pixel(x, y))
               for y in range(91, 110) for x in range(55, 81)), face.expression
    pic.paste(rgb(face.color), (0, 0), mask)
    return pic.resize((272, 224), Image.Resampling.NEAREST)


def new_face(name):
    face = FaceEngine(Display())
    face.play(name, hold_ms=-1)
    face.started = now - STYLES[name][1]
    face.next_look = now + 9000
    face.next_blink = now + 2600
    face.update()
    return face


def voice_samples():
    audio = array("h", [0] * (SR*3//4))
    for name in ("HAPPY", "CURIOUS", "CONFUSED"):
        with wave.open(str(WAVS / ("expression_"+name+".wav")), "rb") as wav:
            assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (SR, 1, 2)
            samples = array("h", wav.readframes(wav.getnframes()))
        audio.extend(samples)
        audio.extend([0] * (SR//2))
    audio.extend([0] * SR)
    # Pad to complete video frames and normalize the envelope, not the audio.
    audio.extend([0] * ((-len(audio)) % (SR//FPS)))
    levels = []
    for start in range(0, len(audio), SR//FPS):
        window = audio[start:start+SR//FPS]
        levels.append(math.sqrt(sum(v*v for v in window) / len(window)))
    peak = max(levels) or 1
    smoothed, previous = [], 0
    for value in levels:
        target = 0 if value < peak*.025 else min(1, (value/peak)**.65)
        previous = target if target > previous else previous*.38+target*.62
        if previous < .06:
            previous = 0
        smoothed.append(previous)
    return audio, smoothed


def save_gif(frames, path):
    frames[0].save(path, save_all=True, append_images=frames[1:],
                   duration=1000//FPS, loop=0, disposal=2, optimize=True)


def comparison(audio, levels):
    global now
    now = 0
    random.seed(42)
    face = new_face("neutral")
    frames = []
    for index, level in enumerate(levels):
        now = index*1000//FPS
        face.update()
        canvas = Image.new("RGB", (936, 404), BG)
        d = ImageDraw.Draw(canvas)
        d.text((24, 15), "Digit / a little mouth", font=TITLE, fill=FG)
        d.text((24, 53), "Same eyes, same voice sample. Three mouth styles.", font=SMALL, fill=MUTED)
        for col, (style, title, caption) in enumerate(OPTIONS):
            x = 24+col*304
            d.rounded_rectangle((x-1, 88, x+281, 320), radius=12, fill="#030405")
            canvas.paste(picture(face, style, level, index/FPS), (x+4, 92))
            d.text((x, 332), title, font=LABEL, fill=FG)
            d.text((x, 361), caption, font=SMALL, fill=MUTED)
        frames.append(canvas)
    poster = max(range(len(levels)), key=lambda i: levels[i])
    frames[poster].save(OUT / "digit-mouth-options.png")
    save_gif(frames, OUT / "digit-mouth-options.gif")
    wavpath = OUT / "digit-mouth-sample.wav"
    with wave.open(str(wavpath), "wb") as wav:
        wav.setparams((1, 2, SR, 0, "NONE", "not compressed"))
        wav.writeframes(audio.tobytes())
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        command = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                   "-s", "936x404", "-r", str(FPS), "-i", "pipe:0", "-i", str(wavpath),
                   "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p", "-c:a", "aac",
                   "-movflags", "+faststart", "-shortest", str(OUT / "digit-mouth-with-sound.mp4")]
        process = subprocess.Popen(command, stdin=subprocess.PIPE)
        for frame in frames:
            process.stdin.write(frame.tobytes())
        process.stdin.close()
        if process.wait():
            raise RuntimeError("Video encoding failed")


def expression_motion(levels):
    global now
    now = 0
    names = ("happy", "curious", "confused", "sad", "love", "excited")
    faces = [new_face(name) for name in names]
    frames = []
    for index, level in enumerate(levels[:100]):
        now = index*1000//FPS
        canvas = Image.new("RGB", (912, 632), BG)
        d = ImageDraw.Draw(canvas)
        d.text((24, 12), "Tiny oval / different moods", font=TITLE, fill=FG)
        d.text((24, 49), "The mouth follows the voice; the eyes keep their existing animation.", font=SMALL, fill=MUTED)
        for i, face in enumerate(faces):
            face.update()
            x, y = 16+(i%3)*304, 84+(i//3)*272
            canvas.paste(picture(face, "oval", level, index/FPS), (x, y))
            d.text((x+4, y+232), names[i].capitalize(), font=LABEL, fill=FG)
        frames.append(canvas)
    save_gif(frames, OUT / "digit-mouth-expressions.gif")


def gallery():
    global now
    canvas = Image.new("RGB", (1216, 1176), BG)
    d = ImageDraw.Draw(canvas)
    d.text((24, 15), "Tiny oval / all 13 expressions", font=TITLE, fill=FG)
    d.text((24, 53), "Small opening shown for comparison; sleeping settles to a closed mouth.", font=SMALL, fill=MUTED)
    for index, name in enumerate(EXPRESSIONS):
        now = 700
        face = new_face(name)
        x, y = 16+(index%4)*304, 84+(index//4)*272
        canvas.paste(picture(face, "oval", 0 if name == "sleeping" else .65, .7), (x, y))
        d.text((x+4, y+232), name.capitalize(), font=LABEL, fill=FG)
    canvas.save(OUT / "digit-mouth-all-expressions.png")


def page():
    (OUT / "digit-mouth-preview.html").write_text('''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Digit mouth previews</title><style>
body{background:#11151b;color:#edf1f6;font:17px/1.6 system-ui;margin:32px auto;padding:0 24px;max-width:1040px}
h1{font-size:34px;margin-bottom:4px}p{color:#a7b1c0}video,img{width:100%;border-radius:14px;background:#000;margin:10px 0 26px}
video{display:block}h2{font-size:23px}a{color:#ffbd70}</style>
<h1>A little mouth for Digit</h1><p>Three small mouth styles, with Digit’s existing eyes and colors.</p>
<h2>Compare the styles with sound</h2>
<p>Press play to hear saved Digit voice sounds. The mouth opens with their volume and closes in the gaps.</p>
<video controls playsinline preload="metadata" poster="digit-mouth-options.png"><source src="digit-mouth-with-sound.mp4" type="video/mp4"></video>
<h2>Tiny oval across six animated expressions</h2><img src="digit-mouth-expressions.gif" alt="Digit's animated eyes with a small oval mouth">
<h2>All 13 expressions</h2><img src="digit-mouth-all-expressions.png" alt="All thirteen Digit expressions with the proposed tiny mouth">
<p>Desktop concept previews only. Live speech synchronization and robot installation have not been performed.</p></html>''', encoding="utf-8")


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    audio, levels = voice_samples()
    comparison(audio, levels)
    expression_motion(levels)
    gallery()
    page()
    print("Created mouth preview gallery, animated comparisons, and video with saved audio in", OUT)
