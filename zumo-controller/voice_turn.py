"""One bounded gyro turn. No sleeps, hardware imports, or motor writes."""

import math

MIN_TURN_SPEED = 3000  # User-observed minimum reliable movement effort.
MAX_TURN_SPEED = 3200
WIGGLE_SPEED = 6000
WIGGLE_ACCELERATION = 30000  # 200 ms from zero to full target, through the shared ramp.
WIGGLE_DECELERATION = 30000
SPIN_SPEED = 6000
SPIN_ACCELERATION = 30000
SPIN_DECELERATION = 30000


def movement_target(speed):
    """Apply the torque floor to targets, never to braking/ramp output or zero."""
    if 0 < speed < MIN_TURN_SPEED:
        return MIN_TURN_SPEED
    if -MIN_TURN_SPEED < speed < 0:
        return -MIN_TURN_SPEED
    return speed


class VoiceTurn:
    def __init__(self, gyro, clock, motor_sign=1):
        self.gyro = gyro
        self.clock = clock
        self.motor_sign = motor_sign
        self.active = False
        self.result = None

    def start(self, degrees, now, acceleration=6000):
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
        self.wiggle = False
        self.spin = False
        # Slow dashboard ramps need time to reach breakaway torque. Still bounded.
        self.timeout_ms = 6000 + 2 * ((MIN_TURN_SPEED * 1000 + acceleration - 1) // acceleration)
        self.result = None
        self.active = True

    def start_wiggle(self, now):
        # One gyro reference for the whole gesture; no recalibration or pauses
        # between left, right, and the original heading.
        self.start(15, now, WIGGLE_ACCELERATION)
        self.wiggle = True
        self.wiggle_leg = 0
        self.center_braking = False
        self.timeout_ms = 4500

    def start_spin(self, now, timeout_ms=12000):
        # One unwrapped reference for a complete revolution. The regular
        # relative-turn limit and its +/-180-degree commands stay unchanged.
        self.start(360, now, SPIN_ACCELERATION)
        self.spin = True
        self.spin_braking = False
        self.timeout_ms = timeout_ms

    def cancel(self):
        self.active = False
        self.command = (0, 0)

    def finish(self, result):
        self.cancel()
        self.result = result
        return self.command

    def update(self, now, motor_effort=0):
        if not self.active:
            return (0, 0)
        diff = self.clock.ticks_diff
        if diff(now, self.started) >= self.timeout_ms:
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
                # Average the reference without rejecting normal sensor noise
                # or table vibration as movement. Tracks stay off while sampling.
                if self.spin and max(self.samples) - min(self.samples) > 15:
                    return self.finish("GYRO_UNSTABLE")
                self.bias = sum(self.samples) / len(self.samples)
                self.samples = None
                self.progress_at = now
            elif diff(now, self.started) >= 1500:
                return self.finish("LOOK_GYRO")
            return (0, 0)
        rate -= self.bias
        self.angle += rate * dt / 1000.0
        if self.wiggle:
            return self._update_wiggle(now, rate, motor_effort)
        if self.spin:
            return self._update_spin(now, rate, motor_effort)
        if self.angle * (1 if self.target >= 0 else -1) < -5:
            return self.finish("LOOK_DIRECTION")
        error = self.target - self.angle
        if abs(error) <= (3 if abs(self.target) <= 30 else 8):
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
        elif motor_effort < MIN_TURN_SPEED:
            # No stall verdict while ramping through the motor's dead zone.
            # The absolute deadline still catches repeated unsuccessful ramps.
            self.progress_at = now
        elif diff(now, self.progress_at) >= 700:
            return self.finish("LOOK_STALLED")
        command = max(-MAX_TURN_SPEED, min(MAX_TURN_SPEED, error * 55 - rate * 4))
        if command * error <= 0:
            command = 0  # Brake through the normal ramp before correcting overshoot.
        elif abs(command) < MIN_TURN_SPEED:
            command = MIN_TURN_SPEED if error > 0 else -MIN_TURN_SPEED
        command = int(command) * self.motor_sign
        self.command = (-command, command)
        return self.command

    def _update_spin(self, now, rate, motor_effort):
        diff = self.clock.ticks_diff
        if self.angle < -5:
            return self.finish("SPIN_DIRECTION")
        if self.angle > 390:
            return self.finish("SPIN_OVERSHOOT")
        if self.spin_braking:
            self.command = (0, 0)
            if motor_effort == 0 and abs(rate) < 10:
                if self.settled is None:
                    self.settled = now
                elif diff(now, self.settled) >= 150:
                    return self.finish("DONE")
            else:
                self.settled = None
            return self.command
        if self.angle >= 360:
            # Never normalize 360 to zero or brake before the complete
            # revolution: braking short would need a second motion segment.
            self.spin_braking = True
            self.command = (0, 0)
            return self.command
        if self.angle >= self.progress_angle + 2 or motor_effort < MIN_TURN_SPEED:
            self.progress_angle = self.angle
            self.progress_at = now
        elif diff(now, self.progress_at) >= 700:
            return self.finish("SPIN_STALLED")
        # Reduce speed while still rotating, using measured yaw and actual
        # effort to account for the shared output ramp. Only the final brake
        # reaches zero; no reversal, recalibration or intermediate rest.
        stopping_yaw = max(0, rate) * (motor_effort / (2.0 * SPIN_DECELERATION) + .015)
        speed = MIN_TURN_SPEED if 360 - self.angle <= stopping_yaw + 30 else SPIN_SPEED
        command = speed * self.motor_sign
        self.command = (-command, command)
        return self.command

    def _update_wiggle(self, now, rate, motor_effort):
        diff = self.clock.ticks_diff
        if (self.wiggle_leg == 0 and self.angle < -5) or abs(self.angle) > 60:
            return self.finish("LOOK_DIRECTION")
        if abs(self.angle - self.progress_angle) >= 2 or motor_effort < MIN_TURN_SPEED:
            self.progress_angle = self.angle
            self.progress_at = now
        elif diff(now, self.progress_at) >= 700:
            return self.finish("LOOK_STALLED")

        # Start reversing before the endpoint: the ramp must first brake the
        # current track output to zero. Estimate that remaining yaw from the
        # measured rate and actual motor effort, with one control tick of lead.
        stopping_yaw = rate * (motor_effort / (2.0 * WIGGLE_DECELERATION) + .015)
        projected = self.angle + stopping_yaw
        if ((self.wiggle_leg == 0 and projected >= 15) or
                (self.wiggle_leg == 1 and projected <= -15)):
            self.wiggle_leg += 1
            self.progress_at = now
            self.progress_angle = self.angle

        if self.wiggle_leg < 2:
            command = WIGGLE_SPEED if self.wiggle_leg == 0 else -WIGGLE_SPEED
        else:
            error = -self.angle
            if self.center_braking and motor_effort == 0 and abs(rate) < 10:
                self.center_braking = False
            if abs(error) <= 3:
                self.center_braking = True
                if motor_effort == 0 and abs(rate) < 10:
                    if self.settled is None:
                        self.settled = now
                    elif diff(now, self.settled) >= 100:
                        return self.finish("DONE")
                else:
                    self.settled = None
            else:
                self.settled = None
                if error * rate > 0 and abs(error) <= abs(stopping_yaw) + 1:
                    self.center_braking = True
            speed = MAX_TURN_SPEED if abs(error) < 8 else WIGGLE_SPEED
            command = 0 if self.center_braking else speed if error > 0 else -speed
        command *= self.motor_sign
        self.command = (-command, command)
        return self.command
