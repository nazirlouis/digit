"""Local review from Digit's production renderer and exported production audio.

Run xiao-sense/tests/run_emotions.ps1 first. This script never contacts hardware.
Action storyboards illustrate reactions; travel and perception time are condensed.
"""
from array import array
import hashlib
import json
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
sys.path[:0] = [str(ROOT / "zumo-controller"), str(ROOT / "zumo-controller/tests")]
from digit_face_engine import FaceEngine, EXPRESSIONS, STYLES
from reaction_catalog import WAKE_TIMING
from test_face_engine import Display

WAVS = ROOT / "xiao-sense/test-artifacts/emotions"
OUT = ROOT / "xiao-sense/test-artifacts/voice-concepts/personality"
OUT.mkdir(parents=True, exist_ok=True)
FFMPEG, FFPROBE = shutil.which("ffmpeg"), shutil.which("ffprobe")
assert FFMPEG and FFPROBE, "FFmpeg and FFprobe must be installed locally."
FPS, SR = 25, 16000
now = 0
time.ticks_ms = lambda: now
time.ticks_diff = lambda a, b: a - b
time.ticks_add = lambda a, b: a + b

DESCRIPTIONS = {
    "neutral": "Quiet, relaxed eyes. Digit does not need to make a sound every time he settles.",
    "happy": "The friendly whistle and chatter, answered by a warm eye smile.",
    "sad": "A small descending whistle and a softer, slower recovery.",
    "mad": "The approved grumpy protest with red, narrowed eyes.",
    "serious": "A restrained acknowledgment for a focused little robot.",
    "curious": "Searching eyes and a rising whistle that sounds like a question.",
    "surprised": "A quick upward chirp as the eyes pop open.",
    "sleeping": "One soft settling phrase, then silence while the eyes stay asleep.",
    "confused": "Violet question marks with hesitant chatter and a final question.",
    "sleepy": "A slowing warble and heavy eyelids, followed by a gentle recovery.",
    "excited": "Fast droid chatter, wide yellow eyes, and a happy bounce.",
    "crying": "A short electronic whimper with tears. Available for deliberate reactions.",
    "love": "Gentle paired whistles and pulsing pink heart eyes.",
}


def wav_samples(path):
    with wave.open(str(path), "rb") as data:
        assert (data.getframerate(), data.getnchannels(), data.getsampwidth()) == (SR, 1, 2)
        return array("h", data.readframes(data.getnframes()))


def write_wav(path, samples):
    with wave.open(str(path), "wb") as data:
        data.setparams((1, 2, SR, 0, "NONE", "not compressed"))
        data.writeframes(samples.tobytes())


def picture(face):
    c = face.color
    rgb = (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    frame = Image.new("RGB", (136, 112))
    frame.putdata([rgb if face.display.pixel(x, y) else (0, 0, 0)
                   for y in range(112) for x in range(136)])
    return frame.resize((544, 448), Image.Resampling.NEAREST)


def beat(face, label, voice=True, quiet_ms=0, wake=False):
    path = WAVS / ("wake.wav" if wake else f"expression_{face.upper()}.wav")
    samples = wav_samples(path) if voice else array("h")
    duration = max(quiet_ms, math.ceil(len(samples) / 640) * 40)
    return {"face": face, "label": label, "wake": wake, "samples": samples,
            "duration_ms": duration, "voice": voice}


def render(key, title, description, group, beats):
    global now
    random.seed(7)
    now = 0
    face = FaceEngine(Display())
    starts, stages, cursor = {}, [], 160
    for item in beats:
        starts[cursor] = item
        stages.append({"at_ms": cursor, "label": item["label"]})
        cursor += item["duration_ms"] + 200
    total_ms = cursor + 400
    total_frames = total_ms // 40
    samples = array("h", [0]) * (total_ms * 16)
    for start, item in starts.items():
        samples[start * 16:start * 16 + len(item["samples"])] = item["samples"]
    wav = OUT / f"{key}.wav"
    write_wav(wav, samples)
    command = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
               "-pixel_format", "rgb24", "-video_size", "544x448", "-framerate", str(FPS),
               "-i", "pipe:0", "-i", str(wav), "-c:v", "libx264", "-preset", "fast",
               "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k",
               "-t", str(total_ms / 1000), "-movflags", "+faststart", str(OUT / f"{key}.mp4")]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    poster = None
    try:
        for index in range(total_frames):
            now = index * 40
            if now in starts:
                item = starts[now]
                face.play(item["face"])
                if item["wake"]:
                    face.enter_ms, face.hold_ms, face.exit_ms = WAKE_TIMING
                # A silent task face should remain readable through this storyboard beat.
                elif not item["voice"]:
                    face.hold_ms = max(0, item["duration_ms"] - face.enter_ms)
            face.update()
            assert not face.display.clipped, f"Clipped face: {key} at {now}ms"
            frame = picture(face)
            first = beats[0]
            if poster is None and now >= 160 + STYLES[first["face"]][1] + 200:
                poster = frame.copy()
                poster.save(OUT / f"{key}.png")
            process.stdin.write(frame.tobytes())
        process.stdin.close()
        error = process.stderr.read().decode(errors="replace")
        if process.wait() != 0:
            raise RuntimeError(error)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if poster is None:
        poster = frame.copy()
        poster.save(OUT / f"{key}.png")
    if group == "faces" and key not in ("sleeping", "neutral"):
        assert face.expression == "neutral", key
    metadata = json.loads(subprocess.check_output([FFPROBE, "-v", "error", "-show_streams", "-of", "json", str(OUT / f"{key}.mp4")]))
    streams = {s["codec_type"]: s for s in metadata["streams"]}
    assert streams["video"]["codec_name"] == "h264" and streams["audio"]["codec_name"] == "aac"
    assert abs(float(streams["video"]["duration"]) * 1000 - total_ms) < 50
    assert abs(float(streams["audio"]["duration"]) * 1000 - total_ms) < 80
    print(f"Rendered {title}: {total_ms / 1000:.2f}s", flush=True)
    return {"id": key, "title": title, "description": description, "group": group,
            "duration_ms": total_ms, "stages": stages}, poster


def main():
    manifest = {"clips": [], "fps": FPS, "source": "Production face renderer and exported production synthesizer",
                "catalog_sha256": hashlib.sha256((ROOT / "reactions/digit_reactions.json").read_bytes()).hexdigest()}
    posters = []
    for name in EXPRESSIONS:
        voice = name != "neutral"
        item = beat(name, "Quiet idle" if name == "neutral" else "One reaction, then a quiet settle",
                    voice=voice, quiet_ms=2400 if name in ("neutral", "sleeping") else 0)
        clip, poster = render(name, name.capitalize(), DESCRIPTIONS[name], "faces", [item])
        manifest["clips"].append(clip)
        posters.append((name, poster))
    clip, _ = render("wake", "You called?", "A brief wake acknowledgment, followed by a quiet listening window.", "moments",
                     [beat("happy", "I heard my name", wake=True), beat("neutral", "Quiet and ready to listen", voice=False, quiet_ms=1200)])
    manifest["clips"].append(clip)
    stories = [
        ("come-say-hello", "Come say hello", [
            beat("curious", "Looking for you"), beat("happy", "There you are!"),
            beat("happy", "Approaching smoothly · travel shortened", voice=False, quiet_ms=1400),
            beat("love", "Arrived · happy to see you")]),
        ("whats-this", "What's this?", [
            beat("curious", "Let me get a good look"),
            beat("surprised", "Oh! I recognize that"), beat("happy", "That is a battery!")]),
        ("find-a-way-out", "Find a way out", [
            beat("curious", "Let's explore"), beat("serious", "Moving toward the wall · travel shortened", voice=False, quiet_ms=1200),
            beat("curious", "Checking the openings"), beat("serious", "Choosing the clearer path · travel shortened", voice=False, quiet_ms=1200),
            beat("happy", "A battery!"), beat("serious", "Approaching the battery · travel shortened", voice=False, quiet_ms=1200),
            beat("excited", "Found the way out!")]),
    ]
    for key, title, beats in stories:
        clip, _ = render(key, title, "Reaction storyboard: searching and travel are condensed; this does not simulate sensors or robot motion.", "actions", beats)
        manifest["clips"].append(clip)
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    shutil.copyfile(ROOT / "tools/personality_preview.html", OUT / "index.html")
    sheet = Image.new("RGB", (1000, 1060), "#f3f6f2")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 18)
    for i, (name, poster) in enumerate(posters):
        x, y = 15 + (i % 4) * 248, 15 + (i // 4) * 262
        sheet.paste(poster.resize((218, 180), Image.Resampling.NEAREST), (x, y))
        draw.text((x, y + 190), name.capitalize(), font=font, fill="#23483e")
    sheet.save(OUT / "all-faces.png")
    print(OUT / "index.html")


if __name__ == "__main__":
    main()
