"""Measured travel scale, separate from encoder velocity calibration."""
import math
import json


def load(path="distance_calibration.json"):
    try:
        with open(path) as source:
            return validate(json.load(source))
    except (OSError, ValueError, KeyError, TypeError):
        return None  # Missing or invalid measurement never enables travel.


def validate(data):
    if not isinstance(data, dict) or data.get("version") != 1:
        raise ValueError("Distance calibration version")
    mm = data["measured_mm"]
    counts = (data["left_counts"], data["right_counts"])
    if not isinstance(mm, (int, float)) or not math.isfinite(mm) or not 100 <= mm <= 3000:
        raise ValueError("Measure at least 100 mm on the actual surface")
    if any(not isinstance(n, (int, float)) or not math.isfinite(n) or n <= 0 for n in counts):
        raise ValueError("Forward encoder counts must be positive")
    scales = tuple(n / mm for n in counts)
    if any(not .1 <= v <= 1000 for v in scales) or max(scales) > min(scales) * 1.25:
        raise ValueError("Distance calibration range or track mismatch")
    return scales
