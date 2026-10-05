"""Replay a local command WAV through Digit's decoder without robot actions."""
import argparse
import json
import time
import urllib.request
import wave
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--url", default="http://10.0.0.127")
    parser.add_argument("--gain", type=int, choices=range(1, 9), default=1)
    args = parser.parse_args()
    with wave.open(str(args.wav), "rb") as source:
        if (source.getframerate(), source.getsampwidth(), source.getnchannels(), source.getcomptype()) != (16000, 2, 1, "NONE"):
            parser.error("Input must be an uncompressed 16 kHz mono signed 16-bit WAV")
        pcm = source.readframes(source.getnframes())
    if not pcm or len(pcm) > 16000 * 7 * 2:
        parser.error("Input must contain audio and be no longer than seven seconds")
    # Preserve complete input, padding only the final frame with silence.
    pcm += bytes((-len(pcm)) % 1024)
    if len(pcm) > 16000 * 7 * 2:
        parser.error("Input exceeds the diagnostic frame limit after padding")
    endpoint = args.url.rstrip("/") + "/api/actions/audio/test"
    request = urllib.request.Request(endpoint + "?gain=%d" % args.gain, data=pcm,
        headers={"Content-Type": "application/octet-stream"}, method="POST")
    with urllib.request.urlopen(request, timeout=8) as response:
        print(response.read().decode(), flush=True)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        with urllib.request.urlopen(endpoint, timeout=5) as response:
            result = json.load(response)
        if result["complete"]:
            print(json.dumps(result, indent=2))
            return
        time.sleep(.3)
    raise TimeoutError("Decoder test did not finish")


if __name__ == "__main__":
    main()
