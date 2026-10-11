"""Record a measured floor run through GET-only dashboard telemetry.

Never commands motors or installs calibration. Run start while stopped, drive
manually along a ruler, stop, then finish with the actual measured distance.
"""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distance_calibration import validate

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("mode", choices=("start", "finish"))
parser.add_argument("--host", default="http://10.0.0.127")
parser.add_argument("--measured-mm", type=float)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
capture = root / "distance_calibration_run.json"
with urllib.request.urlopen(args.host + "/api/zumo/status", timeout=5) as response:
    status = json.load(response)
if not status["connected"] or status["manual_driving"] or status["motion_active"] or status["left"] or status["right"] or abs(status["encoder_left_cps"]) > 50 or abs(status["encoder_right_cps"]) > 50:
    raise SystemExit("Stop Digit and wait for both tracks to rest first.")
if status["reply_age_ms"] > 250 or not status["encoder_control"]:
    raise SystemExit("Fresh calibrated encoder feedback required.")
current = {"boot": status["motion_boot"], "left": status["encoder_left_count"], "right": status["encoder_right_count"]}
if args.mode == "start":
    capture.write_text(json.dumps(current, indent=2) + "\n", encoding="utf-8")
    print("Start counts saved. Drive straight forward manually, stop, and measure the actual travel on the floor.")
else:
    initial = json.loads(capture.read_text(encoding="utf-8"))
    if initial["boot"] != current["boot"]:
        raise SystemExit("Zumo restarted: discard this run and start again.")
    data = {"version": 1, "measured_mm": args.measured_mm}
    for track in ("left", "right"):
        data[track + "_counts"] = ((current[track] - initial[track] + 0x80000000) & 0xffffffff) - 0x80000000
    try:
        scales = validate(data)
    except (ValueError, TypeError) as error:
        raise SystemExit(str(error))
    output = root / "distance_calibration.json"
    output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print("Measured calibration saved locally; counts/mm:", scales)
    print("Install distance_calibration.json on the Zumo and restart. Validate shorter forward/reverse runs before AI movement.")
