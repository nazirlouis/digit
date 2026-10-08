"""Persistent local motion jobs. No hardware writes occur outside UARTController.

The wire protocol is documented in docs/TWO_BRAIN_PROTOCOL.md. A job is
bounded locally; heartbeats keep the supervisor lease alive, never the job
deadline. Encoder counts are deliberately not presented as millimetres.
"""
import math
import os


HEARTBEAT_MS = 750
SETTLE_MS = 150
SETTLE_CPS = 50
MEASURE_SAMPLES = 3
CRC_NIBBLES = (0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
               0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef)


def crc16(text):
    crc = 0xffff
    for value in text.encode("ascii"):
        # Two nibble lookups instead of eight Python bit iterations. Status
        # framing must not consume the motor loop's timing budget on RP2040.
        crc = ((crc << 4) ^ CRC_NIBBLES[((crc >> 12) ^ (value >> 4)) & 15]) & 0xffff
        crc = ((crc << 4) ^ CRC_NIBBLES[((crc >> 12) ^ value) & 15]) & 0xffff
    return crc


def frame(wire, body):
    return "{} {} *{:04X}".format(wire, body, crc16(body))


class MotionJobs:
    def __init__(self, controller):
        self.c = controller
        self.clock = controller.clock
        self.boot = "".join("{:02x}".format(n) for n in os.urandom(4))
        self.session = 0
        self.seq = 0
        self.armed = False
        self.state = "DISARMED"
        self.reason = "NONE"
        self.kind = None
        self.payload = None
        self.heartbeat = None
        self.heartbeat_counter = 0
        self.started = None
        self.limit = 0
        self.settled = None
        self.acked = True
        self.reported = None
        self.heading = 0.0
        self.turn_origin = 0.0
        self.bias = None
        self.bias_at = None
        self.gyro_at = None
        self.bias_count = 0
        self.bias_total = 0.0
        self.bias_min = self.bias_max = 0.0
        self.target_heading = 0.0
        self.revision = 0
        self.left = self.right = 0
        self.acceleration = 3000
        self.stop_mm = 50
        self.last_range_at = None
        self.last_distance = None
        self.closing_mm_s = 0.0
        self.closing_rates = [0.0, 0.0, 0.0]
        self.closing_index = 0
        self.approach_restarts = 0
        self.measured = -1
        self.measured_at = None
        self.measure_samples = []

    @property
    def active(self):
        return self.state in ("CALIBRATING", "RUNNING", "BRAKING", "SETTLING")

    def snapshot(self, wire="0", now=None):
        c = self.c
        now = self.clock.ticks_ms() if now is None else now
        distance = self.measured if self.reason == "MEASURED" else c.distance_mm
        at = self.measured_at if self.reason == "MEASURED" else c.range_at
        age = min(65535, max(0, self.clock.ticks_diff(now, at))) if at is not None else 65535
        target = c.speed_control.targets if c.speed_control else (0, 0)
        body = "M2 {} {} {} {} {} {} {} {} GAP={} OVERRUN={} DSUS={} TL={} TR={}".format(
            self.boot, self.session, self.seq, self.state, self.reason, distance,
            int(self.heading * 10), age, c.max_loop_gap_ms, c.loop_overruns,
            getattr(c.face, "max_slice_us", 0), int(target[0]), int(target[1]))
        c._send(frame(wire, body))
        self.reported = now

    def cancel(self, reason="CANCELLED", fault=False, smooth=False):
        # Keep session + high-water job id after disarming. Old traffic cannot
        # re-arm or restart the most recently cancelled session.
        was_active = self.active or self.armed
        guarded_brake = smooth and self.kind == "DRIVE" and self.c._moving()
        self.armed = False
        self.kind = None
        self.state = "FAULT" if fault else "DISARMED"
        self.reason = reason
        self.acked = False
        self.c._stop(smooth=smooth)
        if guarded_brake:
            # The session is closed immediately but local ToF protection stays
            # active until the normal output ramp reaches zero.
            self.c.safe_motion = True
            self.c.approach_stop_mm = self.stop_mm
            self.c.approach_arrived = True
        if was_active:
            self.snapshot()

    def _finish(self, reason):
        self.c._stop()
        self.state = "DONE"
        self.reason = reason
        self.kind = None
        self.acked = False
        self.snapshot()

    def _error(self, wire, reason):
        self.c._send(wire + " ERR M2_" + reason)

    def handle(self, wire, line, now):
        # CRC is checked before parsing any command that can affect motion.
        try:
            body, checksum = line.rsplit(" *", 1)
            if len(checksum) != 4 or crc16(body) != int(checksum, 16):
                return self._error(wire, "CRC")
            fields = body.split()
            if len(fields) < 5 or fields[0] != "M2":
                return self._error(wire, "FORMAT")
            boot, session, seq, verb = fields[1], int(fields[2]), int(fields[3]), fields[4]
            args = [int(n) for n in fields[5:]]
            if not 1 <= session <= 0xffffffff or not 0 <= seq <= 65535:
                return self._error(wire, "ID")
        except (ValueError, UnicodeError):
            return self._error(wire, "FORMAT")
        if boot != self.boot:
            return self._error(wire, "BOOT")
        if verb == "ARM":
            if seq or args:
                return self._error(wire, "ARGUMENT")
            if session == self.session:
                if self.armed:
                    self.snapshot(wire, now)
                else:
                    self._error(wire, "SESSION_CLOSED")
                return
            if session < self.session:
                return self._error(wire, "SESSION_CLOSED")
            if self.armed or self.c._moving() or self.c.look_id is not None:
                return self._error(wire, "BUSY")
            if not self.c.ready or self.c.calibration is not None:
                return self._error(wire, "NOT_READY")
            if not self.c.speed_control or not self.c.voice_turn:
                return self._error(wire, "FEEDBACK_REQUIRED")
            if not self._stopped(now):
                return self._error(wire, "NOT_STOPPED")
            self.session, self.seq = session, 0
            self.payload = None
            self.armed = True
            self.heartbeat = now
            self.heartbeat_counter = 0
            self.state, self.reason = "ARMED", "NONE"
            self.acked = True
            self.snapshot(wire, now)
            return
        if session != self.session:
            return self._error(wire, "SESSION")
        if verb == "ACK" and not args and seq == self.seq:
            self.acked = True
            return
        if not self.armed:
            return self._error(wire, "DISARMED")
        if verb == "HB" and len(args) == 1 and not seq and 0 < args[0] <= 0xffffffff:
            if args[0] > self.heartbeat_counter:
                self.heartbeat_counter = args[0]
                self.heartbeat = now
            return  # No heartbeat ACK flood; STATUS exposes the session.
        if verb == "CANCEL" and not args and seq in (0, self.seq):
            self.cancel()
            self.snapshot(wire, now)
            return
        if verb == "UPDATE":
            if seq == self.seq and (self.state == "DONE" or
                    (self.kind == "DRIVE" and self.state in ("BRAKING", "SETTLING"))):
                # A visual correction can have been queued before the host
                # learns we are braking. It must neither resume nor fail the
                # locally controlled arrival.
                self.snapshot(wire, now)
                return
            if (seq != self.seq or self.kind != "DRIVE" or self.state not in ("RUNNING", "CALIBRATING") or
                    len(args) != 3 or not 0 < args[0] <= 0x7fffffff or
                    not all(3000 <= v <= 3200 for v in args[1:])):
                return self._error(wire, "UPDATE")
            revision, left, right = args
            if revision < self.revision:
                return self._error(wire, "STALE")
            if revision == self.revision and (left, right) != (self.left, self.right):
                return self._error(wire, "SEQ")
            if revision > self.revision:
                # Capture a new heading when visual steering returns to straight.
                if left == right and self.left != self.right:
                    self.target_heading = self.heading
                self.revision, self.left, self.right = revision, left, right
            self.snapshot(wire, now)
            return
        if seq == self.seq and self.payload is not None:
            if self.payload == fields[4:]:
                self.snapshot(wire, now)
            else:
                self._error(wire, "SEQ")
            return
        if not seq or seq <= self.seq:
            return self._error(wire, "STALE")
        if self.active or self.c._moving():
            return self._error(wire, "BUSY")
        if not self.c.ready or not self._stopped(now):
            return self._error(wire, "NOT_STOPPED")
        if verb == "DRIVE":
            if (len(args) != 5 or not all(3000 <= v <= 3200 for v in args[:2]) or
                    not 500 <= args[2] <= 6000 or args[3] not in (50, 100) or not 100 <= args[4] <= 60000):
                return self._error(wire, "ARGUMENT")
            if not self.c._range_valid(now):
                return self._error(wire, "RANGE_UNAVAILABLE")
        elif verb == "TURN":
            if (len(args) != 3 or not -180 <= args[0] <= 180 or not 500 <= args[1] <= 6000 or
                    not 100 <= args[2] <= 30000):
                return self._error(wire, "ARGUMENT")
        elif verb == "MEASURE":
            if len(args) != 1 or not 100 <= args[0] <= 5000:
                return self._error(wire, "ARGUMENT")
        else:
            return self._error(wire, "VERB")
        self.c._stop()
        self.seq, self.payload = seq, fields[4:]
        self.kind, self.state, self.reason = verb, "RUNNING", "ACCEPTED"
        self.started, self.limit = now, args[-1]
        self.settled = None
        self.acked = False
        self.revision = 0
        self.measured, self.measured_at = -1, None
        self.last_range_at = self.c.range_at
        self.last_distance = self.c.distance_mm
        self.closing_mm_s = 0.0
        self.closing_rates = [0.0, 0.0, 0.0]
        self.closing_index = 0
        self.approach_restarts = 0
        if verb == "DRIVE":
            self.left, self.right, self.acceleration, self.stop_mm = args[:4]
            self.target_heading = self.heading
            self.gyro_at = now
            if self.bias is None or self.clock.ticks_diff(now, self.bias_at) >= 60000:
                self.state = "CALIBRATING"
                self.bias_count, self.bias_total = 0, 0.0
            else:
                self._drive(now, 0)
        elif verb == "TURN":
            self.turn_origin = self.heading
            self.acceleration = args[1]
            self.c.voice_turn.start(args[0], now, args[1])
            self.c.voice_turn.timeout_ms = args[2]
        else:
            self.state = "SETTLING"
            self.measure_samples = []
        self.snapshot(wire, now)

    def _stopped(self, now):
        c = self.c
        return (not c._moving() and not c.braking and c.speed_control is not None and
                self.clock.ticks_diff(now, c.speed_control.at) <= 150 and
                all(abs(v) <= SETTLE_CPS for v in c.speed_control.velocity))

    def _settled(self, now):
        if not self._stopped(now):
            self.settled = None
            return False
        if self.settled is None:
            self.settled = now
        return self.clock.ticks_diff(now, self.settled) >= SETTLE_MS

    def _gyro(self, now):
        gyro = self.c.voice_turn.gyro
        dt = self.clock.ticks_diff(now, self.gyro_at)
        if dt > 150 or dt < 0:
            raise RuntimeError("GYRO_STALE")
        if not gyro.data_ready():
            return None
        gyro.read()
        rate = gyro.last_reading_dps[2]
        if not math.isfinite(rate) or abs(rate) > 500:
            raise RuntimeError("GYRO_RANGE")
        self.gyro_at = now
        if self.state == "CALIBRATING":
            if not self._stopped(now):
                raise RuntimeError("CALIBRATION_MOVING")
            if not self.bias_count:
                self.bias_min = self.bias_max = rate
            self.bias_min, self.bias_max = min(self.bias_min, rate), max(self.bias_max, rate)
            self.bias_count += 1
            self.bias_total += rate
            if self.bias_count >= 80 and self.clock.ticks_diff(now, self.started) >= 400:
                if self.bias_max - self.bias_min > 15:
                    raise RuntimeError("GYRO_UNSTABLE")
                self.bias, self.bias_at = self.bias_total / self.bias_count, now
                self.state = "RUNNING"
            return None
        rate -= self.bias
        self.heading += rate * dt / 1000.0
        return rate

    def _drive(self, now, rate):
        left, right = self.left, self.right
        if left == right:
            # Correct yaw by increasing the lagging track, preserving the
            # measured breakaway floor. No guessed track width is involved.
            error = self.target_heading - self.heading
            correction = max(-200, min(200, error * 35 - rate * 5)) * self.c.voice_turn.motor_sign
            if correction > 0:
                boost = min(3200 - right, int(correction))
                right += boost
                left = max(3000, left - int(correction) + boost)
            else:
                boost = min(3200 - left, -int(correction))
                left += boost
                right = max(3000, right + int(correction) + boost)
        self.c._target_motion(left, right, now, self.acceleration)

    def _range_motion(self, now):
        c = self.c
        if not c._range_valid(now):
            self.cancel("RANGE_UNAVAILABLE", True)
            return False
        if c.distance_mm <= self.stop_mm - 20:
            self.cancel("OBSTACLE", True)
            return False
        if c.range_at != self.last_range_at:
            if self.last_range_at is not None and self.last_distance is not None:
                dt = self.clock.ticks_diff(c.range_at, self.last_range_at)
                if 0 < dt <= 250:
                    measured = max(0, (self.last_distance - c.distance_mm) * 1000.0 / dt)
                    # A single valid-status range jump is not a measured robot
                    # velocity. The median requires two consistent samples.
                    # ToF supplies millimetres; no encoder geometry is guessed.
                    self.closing_rates[self.closing_index] = measured
                    self.closing_index = (self.closing_index + 1) % 3
                    median = sorted(self.closing_rates)[1]
                    self.closing_mm_s = max(median, self.closing_mm_s * .7)
            self.last_range_at, self.last_distance = c.range_at, c.distance_mm
        if self.state == "RUNNING":
            brake_seconds = max(abs(c.left), abs(c.right)) / 12000.0
            sample_age = max(0, self.clock.ticks_diff(now, c.range_at)) / 1000.0
            lead = self.closing_mm_s * (brake_seconds * .5 + sample_age + .060)
            if c.distance_mm <= self.stop_mm + lead:
                c._stop(smooth=True)
                self.state, self.reason, self.settled = "BRAKING", "WALL_BRAKING", None
                self.snapshot(now=now)
        return True

    def tick(self, now):
        c = self.c
        if self.armed:
            if not c.ready or c.button_c.is_pressed():
                self.cancel((c.fault if c.fault != "NONE" else "NOT_READY") if not c.ready else "BUTTON_C", True)
            elif self.clock.ticks_diff(now, self.heartbeat) >= HEARTBEAT_MS:
                self.cancel("HEARTBEAT_TIMEOUT", True)
        if not self.active:
            if self.session and not self.acked and (self.reported is None or self.clock.ticks_diff(now, self.reported) >= 250):
                self.snapshot(now=now)
            return
        if self.clock.ticks_diff(now, self.started) >= self.limit:
            self.cancel("JOB_TIMEOUT", True)
            return
        if self.kind == "DRIVE":
            if not self._range_motion(now):
                return
            try:
                rate = self._gyro(now)
            except Exception as error:
                reason = str(error)
                self.cancel(reason if reason in ("GYRO_STALE", "GYRO_RANGE", "CALIBRATION_MOVING", "GYRO_UNSTABLE") else "GYRO_IO", True)
                return
            if self.state == "BRAKING":
                if self._settled(now):
                    if c.distance_mm <= self.stop_mm + 10:
                        self._finish("WALL_STOPPED")
                    elif self.approach_restarts >= 6:
                        self.cancel("APPROACH_UNSETTLED", True)
                    else:
                        # Prediction may stop short due a range discontinuity
                        # or conservative lead. Continue the same bounded job;
                        # never tell the XIAO we reached a still-distant wall.
                        self.approach_restarts += 1
                        self.state, self.reason = "RUNNING", "APPROACHING"
                        self.closing_mm_s = 0.0
                        self.closing_rates = [0.0, 0.0, 0.0]
                        self.last_range_at, self.last_distance = c.range_at, c.distance_mm
                        self.settled = None
                        self.snapshot(now=now)
                return
            if self.state == "RUNNING" and rate is not None:
                self._drive(now, rate)
        elif self.kind == "TURN":
            if self.state == "SETTLING":
                try:
                    self._gyro(now)
                except Exception:
                    self.cancel("GYRO_IO", True)
                    return
                if self._settled(now):
                    self._finish("TURN_DONE")
                return
            turn = c.voice_turn
            left, right = turn.update(now, max(abs(c.left), abs(c.right)))
            self.heading = self.turn_origin + turn.angle
            if turn.result:
                if turn.result != "DONE":
                    self.cancel(turn.result, True)
                else:
                    self.bias, self.bias_at = turn.bias, now
                    self.gyro_at = now
                    c._stop(smooth=True)
                    self.state, self.reason, self.settled = "SETTLING", "TURN_SETTLING", None
            else:
                c._target_motion(left, right, now, self.acceleration)
        elif self.kind == "MEASURE":
            if not self._settled(now):
                self.last_range_at = c.range_at
                self.measure_samples = []
            elif (c._range_valid(now) and c.range_at != self.last_range_at and
                    self.clock.ticks_diff(c.range_at, self.settled) > SETTLE_MS):
                self.last_range_at = c.range_at
                self.measure_samples.append(c.distance_mm)
                if len(self.measure_samples) == MEASURE_SAMPLES:
                    self.measured = sorted(self.measure_samples)[1]
                    self.measured_at = now
                    self._finish("MEASURED")
