"""Render the production face with WAVs exported by run_emotions.ps1.

Desktop-only timing regression and audiovisual review; no controller access.
Requires Pillow and FFmpeg. Output lives in ignored XIAO test-artifacts.
"""
import html
import math
import pathlib
import random
import shutil
import subprocess
import sys
import time
import wave

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "zumo-controller"), str(ROOT / "zumo-controller/tests")]
from digit_face_engine import FaceEngine, EXPRESSIONS, STYLES
from test_face_engine import Display
from wake_reaction import DURATION_MS

WAVS = ROOT / "xiao-sense/test-artifacts/emotions"
OUT = ROOT / "xiao-sense/test-artifacts/expression-sync"
OUT.mkdir(parents=True, exist_ok=True)
ffmpeg = shutil.which("ffmpeg")
if not ffmpeg:
    raise RuntimeError("FFmpeg is required to mux the previews")
now = 0
time.ticks_ms = lambda: now
time.ticks_add = lambda a, b: a+b
time.ticks_diff = lambda a, b: a-b
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 20)
small = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 14)


def duration(wav):
    with wave.open(str(wav)) as data:
        assert data.getframerate() == 16000 and data.getnchannels() == 1
        return data.getnframes() * 1000 // data.getframerate()


def picture(face, title, stamp, total):
    color = face.color
    rgb = (((color >> 11) & 31)*255//31, ((color >> 5) & 63)*255//63, (color & 31)*255//31)
    eyes = Image.new("RGB", (136, 112))
    eyes.putdata([rgb if face.display.pixel(x, y) else (0, 0, 0)
                  for y in range(112) for x in range(136)])
    result = Image.new("RGB", (408, 400), (16, 22, 22))
    result.paste(eyes.resize((408, 336), Image.Resampling.NEAREST), (0, 0))
    draw = ImageDraw.Draw(result)
    draw.text((16, 344), title, font=font, fill="white")
    draw.text((16, 374), f"{min(stamp, total)/1000:.1f}s / {total/1000:.2f}s", font=small, fill="#afc5bb")
    return result


cards = []
for name in (*EXPRESSIONS, "wake"):
    wav = WAVS / ("wake.wav" if name == "wake" else f"expression_{name.upper()}.wav")
    ms = duration(wav)
    _, enter, hold, exit_ms = STYLES["happy" if name == "wake" else name]
    expected = DURATION_MS if name == "wake" else (enter+3200 if name == "sleeping" else enter if hold is None else enter+hold+exit_ms)
    assert ms == expected, f"{name}: face {expected} ms != sound {ms} ms"
    random.seed(7)
    now = 0
    face = FaceEngine(Display())
    if name == "wake":
        face.play("happy", hold_ms=DURATION_MS-enter-exit_ms)
    else:
        face.play(name)
    title = "Wake reply" if name == "wake" else name.capitalize()
    frames = math.ceil((ms+600)/40)
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "408x400",
           "-framerate", "25", "-i", "pipe:0", "-i", str(wav),
           "-c:v", "libx264", "-preset", "fast", "-crf", "20", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "128k", "-af", "apad", "-t", str(frames/25),
           "-movflags", "+faststart", str(OUT / f"{name}.mp4")]
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    poster_saved = False
    try:
        for index in range(frames):
            now = index*40
            face.update()
            frame = picture(face, title, now, ms)
            if not poster_saved and now >= enter+240:
                frame.save(OUT / f"{name}.png")
                poster_saved = True
            process.stdin.write(frame.tobytes())
        process.stdin.close()
        error = process.stderr.read().decode()
        if process.wait() != 0:
            raise RuntimeError(error)
    finally:
        if process.poll() is None:
            process.kill()
    if name not in ("neutral", "sleeping"):
        assert face.expression == "neutral", name
    cards.append(f'<article><video controls playsinline preload="metadata" poster="{name}.png" src="{name}.mp4"></video><h2>{html.escape(title)} <small>{ms/1000:.2f} s</small></h2></article>')
    print(f"PASS {name}: {ms} ms face/audio match; preview rendered", flush=True)

(OUT / "index.html").write_text('''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Digit · Expression &amp; voice previews</title>
<style>*{box-sizing:border-box}body{margin:0;background:#f3f5f1;color:#182b2a;font:16px/1.5 system-ui,sans-serif}main{max-width:1280px;margin:auto;padding:32px}h1{margin:0;font-size:32px}p{color:#526560;max-width:850px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:20px;margin-top:28px}article{background:#fff;border:1px solid #dce4df;border-radius:16px;overflow:hidden}video{width:100%;display:block;background:#000}h2{font-size:18px;margin:14px 18px}small{float:right;color:#526560;font-size:14px;font-weight:400}button{padding:12px 18px;border:0;border-radius:9px;background:#b52d36;color:#fff;font:inherit;cursor:pointer}button:focus-visible{outline:3px solid #126b60;outline-offset:3px}@media(max-width:600px){main{padding:20px}}</style>
<main><h1>Digit's expressions, with a voice to match</h1><p>Press play to watch and hear each reaction. These previews use the actual face renderer and speaker synthesis code. They do not connect to Digit. Real speaker tone and screen timing still need a device check.</p><p>Full expressions settle back to idle. Sleeping takes one quiet breath, then stays asleep. The wake reply is shorter so Digit is ready for your command.</p><button id="stop">Stop all previews</button><div class="grid">'''+"".join(cards)+'''</div></main><script>
const clips=[...document.querySelectorAll('video')];
for(const clip of clips)clip.addEventListener('play',()=>{for(const other of clips)if(other!==clip)other.pause()});
document.getElementById('stop').onclick=()=>{for(const clip of clips){clip.pause();clip.currentTime=0}};
document.addEventListener('visibilitychange',()=>{if(document.hidden)for(const clip of clips)clip.pause()});
</script></html>''', encoding="utf-8")
print(OUT / "index.html")
