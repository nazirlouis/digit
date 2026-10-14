"""Second-round pet mouth concepts. Desktop previews only; no device access."""

import math
import random
import shutil
import subprocess
import wave

from PIL import Image, ImageDraw
import render_mouth_previews as base

OUT = base.OUT
OPTIONS = (("kitten", 'A / Kitten "w"', "Two tiny curves and a soft little jaw"),
           ("puppy", "B / Puppy smile", "A relaxed smile with a gentle opening"),
           ("tiny_u", 'C / Tiny "u"', "Small, shy, and softly rounded"))


def curve(d, points, width=2):
    """Round-capped quadratic path, all at the robot's native resolution."""
    path = []
    for start, control, end in points:
        for i in range(17):
            t = i / 16
            path.append((round((1-t)**2*start[0]+2*(1-t)*t*control[0]+t*t*end[0]),
                         round((1-t)**2*start[1]+2*(1-t)*t*control[1]+t*t*end[1])))
    d.line(path, fill=1, width=width, joint="curve")
    for x, y in (path[0], path[-1]):
        d.ellipse((x, y, x+1, y+1), fill=1)


def mouth(style, level, expression):
    mask = Image.new("1", (136, 112))
    d = ImageDraw.Draw(mask)
    # Nestle the mouth between the bottom curves of the eyes, not at the chin.
    x, y = 68, 85 if expression not in ("happy", "sleeping") else 78
    opened = 0 if level < .14 else max(1, round(5*level))
    if style == "kitten":
        # Keep the familiar w-shaped upper lip through every speaking frame.
        if opened:
            curve(d, [((x-4, y+3), (x-3, y+7+opened), (x, y+7+opened)),
                      ((x, y+7+opened), (x+3, y+7+opened), (x+4, y+3))])
        curve(d, [((x-8, y), (x-7, y+8), (x-3, y+4)),
                  ((x-3, y+4), (x-1, y+3), (x, y+1)),
                  ((x, y+1), (x+1, y+3), (x+3, y+4)),
                  ((x+3, y+4), (x+7, y+8), (x+8, y))])
    elif style == "puppy":
        curve(d, [((x-8, y+1), (x, y+9), (x+8, y))])
        if opened:
            curve(d, [((x-6, y+3), (x-3, y+8+opened), (x, y+8+opened)),
                      ((x, y+8+opened), (x+4, y+8+opened), (x+6, y+2))])
    else:
        curve(d, [((x-4, y+1), (x-5, y+7+opened), (x, y+7+opened)),
                  ((x, y+7+opened), (x+5, y+7+opened), (x+4, y+1))])
        if opened:
            curve(d, [((x-4, y+1), (x, y+3), (x+4, y+1))])
    return mask


def picture(face, style, level):
    pic = base.eyes(face)
    mask = mouth(style, level, face.expression)
    assert all(not (mask.getpixel((x, y)) and face.display.pixel(x, y))
               for y in range(76, 104) for x in range(58, 79)), face.expression
    pic.paste(base.rgb(face.color), (0, 0), mask)
    return pic.resize((272, 224), Image.Resampling.NEAREST)


def video(frames, audio):
    wavpath = OUT / "digit-mouth-sample.wav"
    # Recreate only if the first-round preview audio is absent.
    if not wavpath.exists():
        with wave.open(str(wavpath), "wb") as wav:
            wav.setparams((1, 2, base.SR, 0, "NONE", "not compressed"))
            wav.writeframes(audio.tobytes())
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return
    w, h = frames[0].size
    command = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
               "-s", f"{w}x{h}", "-r", str(base.FPS), "-i", "pipe:0", "-i", str(wavpath),
               "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p", "-c:a", "aac",
               "-movflags", "+faststart", "-shortest", str(OUT / "digit-pet-mouth-with-sound.mp4")]
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    for frame in frames:
        process.stdin.write(frame.tobytes())
    process.stdin.close()
    if process.wait():
        raise RuntimeError("Video encoding failed")


def render():
    base.now = 0
    random.seed(42)
    faces = [base.new_face(name) for name in ("neutral", "happy")]
    audio, raw_levels = base.voice_samples()
    # Softer closing and smaller jaw travel keep the movement gentle.
    levels, previous = [], 0
    for value in raw_levels:
        previous += (value-previous)*(.7 if value > previous else .35)
        if previous < .08:
            previous = 0
        levels.append(previous)
    frames = []
    for index, level in enumerate(levels):
        base.now = index*1000//base.FPS
        for face in faces:
            face.update()
        canvas = Image.new("RGB", (936, 708), base.BG)
        d = ImageDraw.Draw(canvas)
        d.text((24, 16), "Digit / a sweeter little face", font=base.TITLE, fill=base.FG)
        d.text((24, 54), "Tiny pet mouths, tucked closer to the eyes. Gentle movement with his voice.", font=base.SMALL, fill=base.MUTED)
        for col, (style, title, caption) in enumerate(OPTIONS):
            x = 24+col*304
            d.text((x, 95), title, font=base.LABEL, fill=base.FG)
            for row, face in enumerate(faces):
                y = 133+row*268
                d.rounded_rectangle((x-1, y-4, x+281, y+228), radius=12, fill="#030405")
                canvas.paste(picture(face, style, level), (x+4, y))
                d.text((x+6, y+234), "Relaxed" if row == 0 else "Happy", font=base.SMALL, fill=base.MUTED)
            d.text((x, 678), caption, font=base.SMALL, fill=base.MUTED)
        frames.append(canvas)
    frames[0].save(OUT / "digit-pet-mouth-options.png")
    frames[max(range(len(levels)), key=lambda i: levels[i])].save(OUT / "digit-pet-mouth-speaking.png")
    base.save_gif(frames, OUT / "digit-pet-mouth-options.gif")
    video(frames, audio)

    # One larger look at the kitten mouth with the existing heart-eye animation.
    base.now = 0
    face = base.new_face("love")
    closeups = []
    for index, level in enumerate(levels[:100]):
        base.now = index*1000//base.FPS
        face.update()
        canvas = Image.new("RGB", (584, 528), base.BG)
        d = ImageDraw.Draw(canvas)
        d.text((24, 12), 'Kitten "w" / affectionate', font=base.TITLE, fill=base.FG)
        canvas.paste(picture(face, "kitten", level).resize((544, 448), Image.Resampling.NEAREST), (20, 64))
        closeups.append(canvas)
    base.save_gif(closeups, OUT / "digit-pet-mouth-love.gif")
    print("Created pet mouth stills, animations, and sound preview in", OUT)


if __name__ == "__main__":
    render()
