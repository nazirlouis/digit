"""One bounded gyro turn. No sleeps, hardware imports, or motor writes."""

import math


class VoiceTurn:
    def __init__(self, gyro, clock, motor_sign=1):
        self.gyro = gyro
        self.clock = clock
        self.motor_sign = motor_sign
        self.active = False
        self.result = None

    def start(self, degrees, now):
        # Positive is robot-left, matching the Zumo's positive gyro Z axis.
        self.target = degrees
        self.angle = 0.0
        self.started = self.last_sample = now
        self.samples = []
        self.bias = None
        self.settled = None
        self.progress_at = now
        self.progress_angle = 0.0
        self.command = (0, 0)
        self.result = None
        self.active = True

    def cancel(self):
        self.active = False
        self.command = (0, 0)

    def finish(self, result):
        self.cancel()
        self.result = result
        return self.command

    def update(self, now):
        if not self.active:
            return (0, 0)
        diff = self.clock.ticks_diff
        if diff(now, self.started) >= 6000:
            return self.finish("LOOK_TIMEOUT")
        try:
            if not self.gyro.data_ready():
                if diff(now, self.last_sample) >= 150:
                    return self.finish("LOOK_GYRO")
                return self.command
            self.gyro.read()
            rate = self.gyro.last_reading_dps[2]
            if not math.isfinite(rate) or abs(rate) > 500:
                return self.finish("LOOK_GYRO")
        except Exception:
            return self.finish("LOOK_GYRO")
        dt = diff(now, self.last_sample)
        self.last_sample = now
        if dt > 150 or dt < 0:
            return self.finish("LOOK_GYRO")
        if self.bias is None:
            self.samples.append(rate)
            if len(self.samples) >= 80 and diff(now, self.started) >= 400:
                if max(self.samples) - min(self.samples) > 3 or abs(sum(self.samples) / len(self.samples)) > 10:
                    return self.finish("LOOK_MOVING")
                self.bias = sum(self.samples) / len(self.samples)
                self.samples = None
                self.progress_at = now
            elif diff(now, self.started) >= 1500:
                return self.finish("LOOK_GYRO")
            return (0, 0)
        rate -= self.bias
        self.angle += rate * dt / 1000.0
        if self.angle * (1 if self.target >= 0 else -1) < -5:
            return self.finish("LOOK_DIRECTION")
        error = self.target - self.angle
        if abs(error) <= 8:
            self.command = (0, 0)
            self.progress_at = now
            if abs(rate) < 10:
                if self.settled is None:
                    self.settled = now
                elif diff(now, self.settled) >= 150:
                    return self.finish("DONE")
            else:
                self.settled = None
            return self.command
        self.settled = None
        if abs(self.angle - self.progress_angle) >= 2:
            self.progress_angle = self.angle
            self.progress_at = now
        elif diff(now, self.progress_at) >= 700:
            return self.finish("LOOK_STALLED")
        command = max(-1800, min(1800, error * 55 - rate * 4))
        if abs(command) < 700:
            command = 700 if error > 0 else -700
        command = int(command) * self.motor_sign
        self.command = (-command, command)
        return self.command
