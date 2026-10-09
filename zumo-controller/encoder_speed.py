"""Local encoder velocity control; commands remain normalized 0..6000 targets.

The calibration contains measured counts/second at PWM 3000 and 6000 for
each track. No guessed wheel diameter, gear ratio, or encoder polarity.
"""
import math


def sign(value):
    return 1 if value > 0 else -1 if value < 0 else 0


class EncoderSpeed:
    def __init__(self, encoders, clock, calibration):
        self.encoders, self.clock = encoders, clock
        if calibration.get("version") != 1:
            raise ValueError("Encoder calibration version")
        self.low = tuple(calibration["low_cps"])
        self.high = tuple(calibration["high_cps"])
        self.reverse_low = tuple(calibration.get("reverse_low_cps", self.low))
        self.reverse_high = tuple(calibration.get("reverse_high_cps", self.high))
        if any(len(values) != 2 for values in (self.low, self.high, self.reverse_low, self.reverse_high)) or any(
                not math.isfinite(v) or not 10 <= v <= 25000
                for v in self.low + self.high + self.reverse_low + self.reverse_high):
            raise ValueError("Encoder calibration range")
        self.min_cps = max(self.low + self.reverse_low)
        self.max_cps = min(self.high + self.reverse_high)
        if self.max_cps <= self.min_cps * 1.1:
            raise ValueError("Encoder calibration has no shared speed range")
        self.counts = encoders.get_counts()
        self.at = clock.ticks_ms()
        self.velocity = [0.0, 0.0]
        self.reset()

    def reset(self):
        self.integral = [0.0, 0.0]
        self.directions = [0, 0]
        self.progress = [None, None]
        self.reverse_since = [None, None]
        self.targets = [0.0, 0.0]

    def rate_target(self, target):
        magnitude = abs(target)
        rate = (self.min_cps * magnitude / 3000.0 if magnitude <= 3000 else
                self.min_cps + (self.max_cps-self.min_cps) * (magnitude-3000) / 3000.0)
        return sign(target) * rate

    def update(self, now, targets, outputs, braking=False, rate_ratios=None):
        dt = self.clock.ticks_diff(now, self.at)
        fresh = dt >= 50
        delta = (0, 0)
        if fresh:
            if dt > 250 and any(outputs):
                raise RuntimeError("ENCODER_STALE")
            try:
                counts = self.encoders.get_counts()
                delta = tuple(((counts[i]-self.counts[i]+0x80000000) & 0xffffffff)
                              - 0x80000000 for i in range(2))
            except Exception:
                raise RuntimeError("ENCODER_IO")
            self.counts, self.at = counts, now
            for i in range(2):
                rate = delta[i] * 1000.0 / dt
                if abs(rate) > 30000:
                    raise RuntimeError("ENCODER_RANGE")
                self.velocity[i] += .5 * (rate-self.velocity[i])
        if braking:
            self.reset()
            return (0, 0)
        result = []
        for i in range(2):
            direction = sign(targets[i])
            if direction != self.directions[i]:
                self.integral[i] = 0.0
                self.progress[i] = self.reverse_since[i] = None
                self.directions[i] = direction
            ratio = rate_ratios[i] if rate_ratios is not None else 1.0
            if not math.isfinite(ratio) or not 0 < ratio <= 1:
                raise ValueError("Encoder rate ratio")
            slow = ratio < 1
            self.targets[i] = self.rate_target(targets[i]) * ratio
            outside = 1 - i
            if (slow and rate_ratios[outside] == 1 and
                    abs(outputs[outside]) >= 3000 and self.velocity[outside] * direction >= 50):
                # If the outside track saturates under load, follow its actual
                # speed instead of widening the orbit by chasing an impossible
                # nominal outside speed. The dashboard still sets its target.
                self.targets[i] = direction * min(abs(self.rate_target(targets[i])),
                                                 self.velocity[outside] * direction) * ratio
            if not direction:
                result.append(0)
                continue
            rate = abs(self.targets[i])
            low = self.low if direction > 0 else self.reverse_low
            high = self.high if direction > 0 else self.reverse_high
            feedforward = 3000 + (rate-low[i]) * 3000 / (high[i]-low[i])
            # Circle's inside track needs a lower measured velocity than the
            # normal torque-floor target. Give it startup torque, then let PI
            # reduce power once encoder motion is confirmed. Other movement
            # retains the normal 3000 minimum.
            moving = sign(outputs[i]) == direction and (abs(outputs[i]) >= 3000 or
                     (slow and self.velocity[i] * direction >= 50))
            if not moving:
                self.integral[i] = 0.0  # Startup/reversal must not wind up the PI loop.
                if not slow:
                    self.progress[i] = self.reverse_since[i] = None
            if fresh and (moving or (slow and self.progress[i] is not None)):
                if self.progress[i] is None or delta[i] * direction >= 2:
                    self.progress[i] = now
                if delta[i] * direction < -2:
                    if self.reverse_since[i] is None:
                        self.reverse_since[i] = now
                    elif self.clock.ticks_diff(now, self.reverse_since[i]) >= 150:
                        raise RuntimeError("ENCODER_DIRECTION")
                else:
                    self.reverse_since[i] = None
                if self.clock.ticks_diff(now, self.progress[i]) >= 700:
                    raise RuntimeError("ENCODER_STALL")
            error = rate - self.velocity[i] * direction
            proportional = 1.5 * error if moving else 0
            if moving and fresh:
                candidate = max(-1000, min(1000, self.integral[i] + error * dt / 1000.0))
                demand = feedforward + proportional + candidate
                lower = 0 if slow else 3000
                if lower <= demand <= 6000 or (demand > 6000 and error < 0) or (demand < lower and error > 0):
                    self.integral[i] = candidate
            lower = 0 if slow and moving else 3000
            power = max(lower, min(6000, feedforward + proportional + self.integral[i]))
            result.append(int(power) * direction)
        return tuple(result)
