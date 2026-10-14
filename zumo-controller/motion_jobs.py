"""Persistent local motion jobs. No hardware writes occur outside UARTController.

The wire protocol is documented in docs/TWO_BRAIN_PROTOCOL.md. A job is
bounded locally; heartbeats keep the supervisor lease alive, never the job
deadline. Encoder counts are deliberately not presented as millimetres.
"""
import math
import os
from distance_calibration import load as load_distance_calibration
from voice_turn import SPIN_ACCELERATION, SPIN_DECELERATION
from local_recovery import LocalRecovery


HEARTBEAT_MS = 750
FRONT_STOP_MM = 50  # Automatic obstacle stop is strictly below 5 cm.
SETTLE_MS = 150
SETTLE_CPS = 50
MEASURE_SAMPLES = 3
PUSH_SPEED = 6000
PUSH_ACCELERATION = PUSH_DECELERATION = 30000
PUSH_POWERED_MS = 2000
PUSH_JOB_MS = 5000
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
        self.circle_speed = 0
        self.circle_goal = 0
        self.circle_progress = 0.0
        self.circle_progress_at = 0
        self.circle_restarts = 0
        self.circle_range_at = None
        self.spin_range_at = None
        self.distance_scale = load_distance_calibration()
        self.travel_counts = None
        self.travel_tracks = [0.0, 0.0]
        self.travelled_mm = 0.0
        self.travel_target = 0
        self.travel_restarts = 0
        self.spin_scene = None
        self.failure_range = None
        self.reverse_only = False
        self.recovery_count = 0
        self.recovery_cause = "NONE"
        self.position_known = True
        self.recovery = LocalRecovery(self)
        self.push_started = None
        self.push_progress_at = [None, None]
        self.push_progress_counts = None

    @property
    def active(self):
        return self.state in ("CALIBRATING", "RUNNING", "BRAKING", "SETTLING", "RECOVERING", "PAUSED")

    def snapshot(self, wire="0", now=None):
        c = self.c
        now = self.clock.ticks_ms() if now is None else now
        distance = self.measured if self.reason == "MEASURED" else c.distance_mm
        at = self.measured_at if self.reason == "MEASURED" else c.range_at
        range_status, raw_distance = c.range_status, c.raw_distance_mm
        if self.failure_range is not None:
            distance, at, range_status, raw_distance = self.failure_range
        age = min(65535, max(0, self.clock.ticks_diff(now, at))) if at is not None else 65535
        target = c.speed_control.targets if c.speed_control else (0, 0)
        body = "M2 {} {} {} {} {} {} {} {} GAP={} OVERRUN={} DSUS={} TL={} TR={} RS={} RAW={}".format(
            self.boot, self.session, self.seq, self.state, self.reason, distance,
            int(self.heading * 10), age, c.max_loop_gap_ms, c.loop_overruns,
            getattr(c.face, "max_slice_us", 0), int(target[0]), int(target[1]), range_status, raw_distance)
        body += " TRAVEL={}".format(int(self.travelled_mm))
        body += " REC={} CAUSE={} KNOWN={}".format(self.recovery_count, self.recovery_cause, int(self.position_known))
        body += " REV={}".format(int(c.edge_reverse.available()))
        retreat = c.edge_retreat
        if retreat.mask or retreat.blocked:
            body += " ER={} BACK={}".format(retreat.mask, max(0, int(retreat.back_mm)))
        if retreat.reject_reason:
            body += " EREJ={} EDETAIL={}".format(retreat.reject_reason, retreat.reject_detail)
        c._send(frame(wire, body))
        self.reported = now

    def cancel(self, reason="CANCELLED", fault=False, smooth=False):
        if fault and reason in ("ENCODER_DIRECTION", "ENCODER_RANGE") and self.c.ready:
            self.recovery_cause = reason
            self.c._latch(reason)
            return
        recoverable = (reason.startswith(("RANGE_", "GYRO_")) or reason in (
            "OBSTACLE", "FEEDBACK_IO", "ENCODER_IO", "ENCODER_STALE", "ENCODER_STALL",
            "TRACK_MISMATCH", "HEADING_ERROR", "TRAVEL_OVERSHOOT", "TRAVEL_INCOMPLETE",
            "CONTROL_STALE", "BRAKE_TIMEOUT", "LOOK_GYRO", "LOOK_TIMEOUT",
            "CIRCLE_STALLED", "CIRCLE_UNSETTLED", "SPIN_INCOMPLETE", "PUSH_TIMEOUT", "JOB_TIMEOUT"))
        if fault and recoverable and self.kind != "DANCE" and not reason.endswith("_PERSISTENT"):
            now = self.clock.ticks_ms()
            if reason == "ENCODER_STALL" and self.kind == "TRAVEL" and not self.recovery.active:
                readings = self.c.last_line_readings
                if readings is not None and self.c.edge_retreat.begin(readings, now, blocked=True):
                    return
            if self.recovery.begin(reason, now):
                return
        # Keep session + high-water job id after disarming. Old traffic cannot
        # re-arm or restart the most recently cancelled session.
        was_active = self.active or self.armed
        if fault and self.failure_range is None:
            self.failure_range = (self.c.distance_mm, self.c.range_at,
                                  self.c.range_status, self.c.raw_distance_mm)
        guarded_brake = smooth and self.kind == "DRIVE" and self.c._moving()
        if self.kind == "EDGE":
            self.c.edge_retreat.lock(fault)
            smooth = False
        self.armed = False
        self.recovery.active = False
        self.kind = None
        self.state = "FAULT" if fault else "DISARMED"
        self.reason = reason
        self.acked = False
        self.c._stop(smooth=smooth)
        self._release_spin_scene()
        if guarded_brake:
            # The session is closed immediately but local ToF protection stays
            # active until the normal output ramp reaches zero.
            self.c.safe_motion = True
            self.c.approach_stop_mm = self.stop_mm
            self.c.approach_arrived = True
        if was_active:
            self.snapshot()

    def _finish(self, reason):
        self.recovery.active = False
        self.c._stop()
        self._release_spin_scene()
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
            if self.c.expression_motion.active:
                self.c._stop()  # A validated new driving session owns the tracks.
            if self.armed or self.c._moving() or self.c.look_id is not None:
                return self._error(wire, "BUSY")
            reverse_only = self.c.edge_reverse.available()
            if (not self.c.ready and not reverse_only) or self.c.calibration is not None:
                return self._error(wire, "NOT_READY")
            if not self.c.speed_control or (not reverse_only and not self.c.voice_turn):
                return self._error(wire, "FEEDBACK_REQUIRED")
            if not self._stopped(now):
                return self._error(wire, "NOT_STOPPED")
            self.session, self.seq = session, 0
            self.reverse_only = reverse_only
            self.failure_range = None
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
                    (self.kind == "DRIVE" and self.state in ("BRAKING", "SETTLING", "RECOVERING", "PAUSED"))):
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
        if self.reverse_only and verb != "BACK":
            return self._error(wire, "EDGE_REVERSE_ONLY")
        if (not self.c.ready and not self.c.edge_reverse.available()) or not self._stopped(now):
            return self._error(wire, "NOT_STOPPED")
        if verb == "BACK":
            if not self.reverse_only or len(args) != 1 or not 10 <= args[0] <= 100:
                return self._error(wire, "EDGE_REVERSE_ONLY")
        elif verb == "DRIVE":
            if (len(args) != 5 or not all(3000 <= v <= 3200 for v in args[:2]) or
                    not 500 <= args[2] <= 6000 or args[3] not in (50, 100) or not 100 <= args[4] <= 60000):
                return self._error(wire, "ARGUMENT")
            if not (self.c._range_valid(now) or self.c._range_no_return(now)):
                return self._error(wire, "RANGE_UNAVAILABLE")
            if self.c._range_valid(now) and self.c.distance_mm < FRONT_STOP_MM:
                return self._error(wire, "OBSTACLE")
        elif verb in ("TRAVEL", "PUSH"):
            if verb == "PUSH" and (len(args) != 1 or not 10 <= args[0] <= 300):
                return self._error(wire, "ARGUMENT")
            if verb == "TRAVEL" and (len(args) not in (4, 5) or not -100 <= args[0] <= 300 or not args[0] or
                    args[1] != 3000 or not 500 <= args[2] <= 6000 or not 100 <= args[3] <= 15000):
                return self._error(wire, "ARGUMENT")
            if len(args) == 5 and (args[4] != 1 or args[0] <= 0 or self.c.edge_retreat.acc is None):
                return self._error(wire, "EDGE_CAPABILITY")
            if self.distance_scale is None or not hasattr(self.c.speed_control, "encoders"):
                return self._error(wire, "DISTANCE_CALIBRATION")
            if not (self.c._range_valid(now) or self.c._range_no_return(now)):
                return self._error(wire, "RANGE_UNAVAILABLE")
            if verb != "PUSH" and args[0] > 0 and self.c._range_valid(now) and self.c.distance_mm < FRONT_STOP_MM:
                return self._error(wire, "OBSTACLE")
            try:
                origin = self.c.speed_control.encoders.get_counts()
            except Exception:
                return self._error(wire, "ENCODER_IO")
        elif verb == "TURN":
            if (len(args) != 3 or not -180 <= args[0] <= 180 or not 500 <= args[1] <= 6000 or
                    not 100 <= args[2] <= 30000):
                return self._error(wire, "ARGUMENT")
        elif verb == "CIRCLE":
            if (len(args) != 4 or not 3000 <= args[0] <= 6000 or args[1] != 3 or
                    not 500 <= args[2] <= 6000 or not 1000 <= args[3] <= 60000):
                return self._error(wire, "ARGUMENT")
            if not (self.c._range_valid(now) or self.c._range_no_return(now)):
                return self._error(wire, "RANGE_UNAVAILABLE")
            if self.c._range_valid(now) and self.c.distance_mm < FRONT_STOP_MM:
                return self._error(wire, "OBSTACLE")
        elif verb == "SPIN":
            if len(args) != 1 or not 1000 <= args[0] <= 30000:
                return self._error(wire, "ARGUMENT")
            if not (self.c._range_valid(now) or self.c._range_no_return(now)):
                return self._error(wire, "RANGE_UNAVAILABLE")
            if self.c._range_valid(now) and self.c.distance_mm < FRONT_STOP_MM:
                return self._error(wire, "OBSTACLE")
        elif verb == "MEASURE":
            if len(args) != 1 or not 100 <= args[0] <= 5000:
                return self._error(wire, "ARGUMENT")
        elif verb == "DANCE":
            if len(args) != 1 or args[0] != 8000:
                return self._error(wire, "ARGUMENT")
            if not getattr(self.c.face, "cooperative", False) or self.c.rgb_leds is None:
                return self._error(wire, "DANCE_DISPLAY_REQUIRED")
            if not (self.c._range_valid(now) or self.c._range_no_return(now)):
                return self._error(wire, "RANGE_UNAVAILABLE")
            if self.c._range_valid(now) and self.c.distance_mm < FRONT_STOP_MM:
                return self._error(wire, "OBSTACLE")
        else:
            return self._error(wire, "VERB")
        self.c._stop()
        self.seq, self.payload = seq, fields[4:]
        self.failure_range = None
        self.kind, self.state, self.reason = verb, "RUNNING", "ACCEPTED"
        self.started, self.limit = now, PUSH_JOB_MS if verb == "PUSH" else 10500 if verb == "BACK" else args[3] if verb == "TRAVEL" else args[-1]
        self.c.edge_retreat.prepare((verb == "TRAVEL" and len(args) == 5) or
                                   (verb == "PUSH" and self.c.edge_retreat.acc is not None))
        self.recovery.active = False
        self.recovery_count, self.recovery_cause, self.position_known = 0, "NONE", True
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
        self.travelled_mm = 0.0
        if verb == "BACK":
            if not self.c.edge_reverse.begin(now, args[0]):
                self.cancel("EDGE_UNAVAILABLE", True)
                return
        elif verb in ("TRAVEL", "PUSH"):
            self.travel_target = args[0]
            self.travel_counts = origin
            self.travel_tracks = [0.0, 0.0]
            self.travel_restarts = 0
            self.left = self.right = PUSH_SPEED if verb == "PUSH" else args[1] if args[0] > 0 else -args[1]
            self.acceleration = PUSH_ACCELERATION if verb == "PUSH" else args[2]
            self.push_started = None
            self.push_progress_at = [None, None]
            self.push_progress_counts = list(origin)
            self.target_heading = self.heading
            self.gyro_at = now
            self.state = "CALIBRATING"
            self.bias_count, self.bias_total = 0, 0.0
        elif verb == "DRIVE":
            self.left, self.right, self.acceleration, self.stop_mm = args[:4]
            self.target_heading = self.heading
            self.gyro_at = now
            if self.bias is None or self.clock.ticks_diff(now, self.bias_at) >= 60000:
                self.state = "CALIBRATING"
                self.bias_count, self.bias_total = 0, 0.0
            else:
                self._drive(now, 0)
        elif verb == "CIRCLE":
            self.circle_speed, laps, self.acceleration = args[:3]
            self.circle_range_at = self.c.range_at
            self.circle_goal = laps * 360
            self.turn_origin = self.heading
            self.circle_progress = 0.0
            self.circle_progress_at = now
            self.circle_restarts = 0
            self.gyro_at = now
            self.state = "CALIBRATING"
            self.bias_count, self.bias_total = 0, 0.0
        elif verb == "TURN":
            self.turn_origin = self.heading
            self.acceleration = args[1]
            self.c.voice_turn.start(args[0], now, args[1])
            self.c.voice_turn.timeout_ms = args[2]
        elif verb == "DANCE":
            self.c.face.show_scene("happy", "NONE", timing=(180, 6500, 300))
            self.c.dance_motion.start(self.c.face.active_scene_sequence, now)
            self.position_known = False  # Timed wiggles do not measure restored heading.
        elif verb == "SPIN":
            self.turn_origin = self.heading
            self.spin_range_at = self.c.range_at
            face = self.c.face
            sequence = getattr(face, "active_scene_sequence", None)
            if (getattr(face, "expression", None) == "happy" and
                    getattr(face, "caption", None) == "FOUND" and
                    hasattr(face, "hold_scene") and hasattr(face, "release_scene") and
                    face.hold_scene(sequence)):
                self.spin_scene = sequence
            self.c.voice_turn.start_spin(now, args[0])
            self.state = "CALIBRATING"
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
        if self.c.edge_retreat.allowed or self.c.edge_retreat.active:
            self.c.edge_retreat.sample_imu(now, gyro.last_reading_dps)
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
        if not self._range_guard(now):
            return False
        # A missed observation retains the previous measured range within the
        # guard's time/clearance window; never turn status -1 into an obstacle.
        if not c._range_valid(now):
            return True
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

    def _range_guard(self, now, allow_no_return=True, obstacle=True):
        c = self.c
        valid = c._range_valid(now)
        if valid and obstacle and c.distance_mm < FRONT_STOP_MM:
            self.cancel("OBSTACLE", True)
            return False
        if valid or (allow_no_return and c._range_no_return(now)):
            at, distance, status = c.range_at, c.distance_mm, c.range_status
        else:
            at, distance, status = c.range_good_at, c.range_good_distance, c.range_good_status
        age = self.clock.ticks_diff(now, at) if at is not None else 65535
        usable = at is not None and 0 <= age < 250 and (distance > 0 or (allow_no_return and status == 2))
        if usable and obstacle and distance > 0 and age > 40 and self.distance_scale is not None and c.left + c.right > 0:
            velocity = max(0, sum(c.speed_control.velocity[i] / self.distance_scale[i] for i in range(2)) / 2)
            lead = velocity * (age / 1000.0 + max(abs(c.left), abs(c.right)) / 24000.0 + .060)
            usable = distance > FRONT_STOP_MM + lead
        if not usable:
            reason = ("RANGE_IO" if c.range_failed or c.tof is None else
                      "RANGE_STALE" if c.range_at is None or self.clock.ticks_diff(now, c.range_at) >= 250 else
                      "RANGE_STATUS_{}".format(c.range_status) if c.range_status not in (0, 2) else "RANGE_UNAVAILABLE")
            self.cancel(reason, True)
            return False
        return True

    def _dance(self, now):
        c = self.c
        if not self._range_guard(now):
            return
        if self.state == "SETTLING":
            if self._settled(now):
                self._finish("DANCE_DONE")
            return
        left, right = c.dance_motion.update(now, c.face)
        if c.dance_motion.result:
            if c.dance_motion.result != "DANCE_DONE":
                self.cancel(c.dance_motion.result, True)
                return
            left = right = 0
            self.state, self.reason, self.settled = "SETTLING", "DANCE_SETTLING", None
        if (left or right) and (left, right) != (c.left, c.right):
            readings = c._read_edge()
            if readings is None:
                self.cancel("DANCE_LINE_IO", True)
                return
            if any(readings[i] >= c.thresholds[i] for i in range(5)):
                if not c.edge_retreat.begin(readings, now):
                    self.cancel("DANCE_EDGE", True)
                    c._latch("EDGE")
                return
        c.target_left, c.target_right = left, right
        c.ramp_left, c.ramp_right = float(left), float(right)
        c.ramp_at = now
        if (left, right) != (c.left, c.right):
            c.motors.set_speeds(left, right)
            c.left, c.right = left, right
        if left or right or c.dance_show.started is not None:
            try:
                c.dance_show.update(now)
            except Exception:
                self.cancel("DANCE_LED_IO", True)
                return
        if (left or right) and self.reason != "DANCING":
            self.reason = "DANCING"
            self.snapshot(now=now)  # Correlated actual first pulse starts XIAO music.

    def tick(self, now):
        c = self.c
        if self.armed:
            reverse_ok = self.reverse_only and (c.edge_reverse.available() or c.edge_reverse.active)
            if (not c.ready and not c.edge_retreat.active and not reverse_ok) or c.button_c.is_pressed():
                self.cancel((c.fault if c.fault != "NONE" else "NOT_READY") if not c.ready else "BUTTON_C", True)
            elif self.clock.ticks_diff(now, self.heartbeat) >= HEARTBEAT_MS:
                self.cancel("HEARTBEAT_TIMEOUT", True)
        if not self.active:
            if self.session and not self.acked and (self.reported is None or self.clock.ticks_diff(now, self.reported) >= 250):
                self.snapshot(now=now)
            return
        if self.clock.ticks_diff(now, self.started) >= self.limit:
            if self.kind == "PUSH":
                self.cancel("PUSH_DEADLINE", True)
                return
            if not self.recovery.active:
                self.cancel("JOB_TIMEOUT", True)
                return
            # The powered deadline never extends. An already stationary
            # recovery may only verify rest and report a partial result.
            self.recovery.resumable = False
            if self.recovery.cause != "JOB_TIMEOUT":
                self.recovery.cause = self.recovery_cause = self.reason = "JOB_TIMEOUT"
        if self.recovery.active:
            self.recovery.tick(now)
            return
        if self.kind == "BACK":
            c.edge_reverse.tick(now)
            self.travelled_mm = -c.edge_reverse.back_mm
            if c.edge_reverse.state == "DONE" and c.ready:
                self._finish("EDGE_BACK_DONE")
        elif self.kind == "EDGE":
            c.edge_retreat.tick(now)
        elif self.kind in ("TRAVEL", "PUSH"):
            self._travel(now)
        elif self.kind == "SPIN":
            self._spin(now)
        elif self.kind == "DANCE":
            self._dance(now)
        elif self.kind == "CIRCLE":
            self._circle(now)
        elif self.kind == "DRIVE":
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
                    if c._range_valid(now) and c.distance_mm <= self.stop_mm + 10:
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
            if not self._range_guard(now):
                return
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

    def _spin(self, now):
        c = self.c
        if not self._range_guard(now):
            return
        turn = c.voice_turn
        if self.state == "SETTLING":
            try:
                rate = self._gyro(now)
            except Exception as error:
                reason = str(error)
                self.cancel(reason if reason in ("GYRO_STALE", "GYRO_RANGE") else "GYRO_IO", True)
                return
            if self.heading - self.turn_origin > 390:
                self.cancel("SPIN_OVERSHOOT", True)
                return
            if rate is None:
                return
            if abs(rate) >= 10:
                self.settled = None
                return
            if self._settled(now):
                if self.heading - self.turn_origin >= 360:
                    self._finish("SPIN_DONE")
                else:
                    self.cancel("SPIN_INCOMPLETE", True)
            return
        if turn.bias is None and not self._stopped(now):
            self.cancel("CALIBRATION_MOVING", True)
            return
        left, right = turn.update(now, max(abs(c.left), abs(c.right)))
        self.heading = self.turn_origin + turn.angle
        if turn.result:
            if turn.result != "DONE":
                self.cancel(turn.result, True)
            else:
                self.bias, self.bias_at = turn.bias, now
                self.gyro_at = now
                c._stop()
                self.state, self.reason, self.settled = "SETTLING", "SPIN_SETTLING", None
        else:
            self.state = "RUNNING" if turn.bias is not None else "CALIBRATING"
            c._target_motion(left, right, now, SPIN_ACCELERATION, SPIN_DECELERATION)

    def _release_spin_scene(self):
        sequence, self.spin_scene = self.spin_scene, None
        if sequence is not None:
            try:
                self.c.face.release_scene(sequence)
            except Exception as error:
                # Motor Stop already happened before display cleanup. A failed
                # optional face update must not interrupt the safety loop.
                print("SPIN FACE:", error)

    def _circle(self, now):
        c = self.c
        if not self._range_guard(now):
            return
        try:
            rate = self._gyro(now)
        except Exception as error:
            reason = str(error)
            self.cancel(reason if reason in ("GYRO_STALE", "GYRO_RANGE", "CALIBRATION_MOVING", "GYRO_UNSTABLE") else "GYRO_IO", True)
            return
        if rate is None:
            return
        progress = self.heading - self.turn_origin
        if progress < -5:
            self.cancel("CIRCLE_DIRECTION", True)
            return
        if self.state == "BRAKING":
            if self._settled(now):
                if progress >= self.circle_goal - 8:
                    self._finish("CIRCLE_DONE")
                elif self.circle_restarts >= 6:
                    self.cancel("CIRCLE_UNSETTLED", True)
                else:
                    self.circle_restarts += 1
                    self.state, self.reason = "RUNNING", "CIRCLING"
                    self.circle_progress_at = now
            return
        effort = max(abs(c.left), abs(c.right))
        if progress >= self.circle_progress + 2 or effort < 3000:
            self.circle_progress, self.circle_progress_at = progress, now
        elif self.clock.ticks_diff(now, self.circle_progress_at) >= 700:
            self.cancel("CIRCLE_STALLED", True)
            return
        # With approximately 3-inch track-center spacing, 3/9-inch inside/
        # outside paths give a 6-inch center path. Scale measured encoder rate,
        # not PWM or the nonlinear dashboard speed, to preserve the 1:3 ratio.
        stopping_yaw = max(0, rate) * (effort / 24000.0 + .020)
        if progress + stopping_yaw >= self.circle_goal:
            c._stop(smooth=True)
            self.state, self.reason, self.settled = "BRAKING", "CIRCLE_BRAKING", None
            self.snapshot(now=now)
        elif c.voice_turn.motor_sign > 0:
            c._target_motion(self.circle_speed, self.circle_speed, now, self.acceleration,
                             rate_ratios=(1.0 / 3.0, 1.0))
        else:
            c._target_motion(self.circle_speed, self.circle_speed, now, self.acceleration,
                             rate_ratios=(1.0, 1.0 / 3.0))

    def _travel(self, now):
        c = self.c
        forward = self.travel_target > 0
        push = self.kind == "PUSH"
        if not self._range_guard(now, obstacle=forward and not push):
            return
        try:
            rate = self._gyro(now)
            self._sample_travel()
        except Exception as error:
            self.cancel(str(error) if str(error).startswith(("GYRO_", "ENCODER_", "CALIBRATION_")) else "FEEDBACK_IO", True)
            return
        direction = 1 if forward else -1
        progress = self.travelled_mm * direction
        goal = abs(self.travel_target)
        if push and self.push_started is not None and self.clock.ticks_diff(now, self.push_started) >= PUSH_POWERED_MS and c._moving():
            self.cancel("PUSH_TIMEOUT", True)
            return
        if progress < -5:
            self.cancel("ENCODER_DIRECTION", True)
            return
        if abs(self.travel_tracks[0]-self.travel_tracks[1]) > max(10, progress*.25):
            self.cancel("TRACK_MISMATCH", True)
            return
        if abs(self.heading-self.target_heading) > 15:
            self.cancel("HEADING_ERROR", True)
            return
        if progress > goal + max(10, goal*.1):
            self.cancel("TRAVEL_OVERSHOOT", True)
            return
        if self.state == "BRAKING":
            if self._settled(now):
                if progress >= goal-3:
                    self._finish("PUSH_DONE" if push else "TRAVEL_DONE")
                elif push or self.travel_restarts >= 3:
                    self.cancel("TRAVEL_INCOMPLETE", True)
                else:
                    self.travel_restarts += 1
                    self.state, self.reason = "RUNNING", "TRAVELLING"
                    self.settled = None
            return
        if self.state != "RUNNING" or rate is None:
            return
        velocity = max(0, sum(c.speed_control.velocity[i] * direction / self.distance_scale[i] for i in range(2))/2)
        deceleration = PUSH_DECELERATION if push else 12000
        lead = velocity * (max(abs(c.left),abs(c.right))/(2.0*deceleration) + .060)
        if push:
            if self._push_stalled(now):
                self.cancel("ENCODER_STALL", True)
                return
        if progress + lead >= goal-1:
            c._stop(smooth=True)
            c.deceleration = deceleration
            self.state, self.reason, self.settled = "BRAKING", "PUSH_BRAKING" if push else "TRAVEL_BRAKING", None
            self.snapshot(now=now)
            return
        error = self.target_heading-self.heading
        correction = max(-200,min(200,(error*35-rate*5)*c.voice_turn.motor_sign))
        if push:
            left = PUSH_SPEED - max(0, correction)
            right = PUSH_SPEED + min(0, correction)
            if self.push_started is None:
                self.push_started = now
            c._target_motion(int(left), int(right), now, PUSH_ACCELERATION, PUSH_DECELERATION)
        else:
            left = direction*max(3000,min(3200,3000-direction*correction))
            right = direction*max(3000,min(3200,3000+direction*correction))
            c._target_motion(int(left),int(right),now,self.acceleration)

    def _sample_travel(self):
        counts = self.c.speed_control.encoders.get_counts()
        for i in range(2):
            delta = ((counts[i] - self.travel_counts[i] + 0x80000000) & 0xffffffff) - 0x80000000
            if abs(delta) > 10000:
                raise RuntimeError("ENCODER_RANGE")
            self.travel_tracks[i] += delta / self.distance_scale[i]
        self.travel_counts = counts
        self.travelled_mm = sum(self.travel_tracks) / 2.0

    def _push_stalled(self, now):
        counts = self.travel_counts
        for i, effort in enumerate((self.c.left, self.c.right)):
            delta = ((counts[i] - self.push_progress_counts[i] + 0x80000000) & 0xffffffff) - 0x80000000
            if effort < 3000 or self.push_progress_at[i] is None or delta >= 2:
                self.push_progress_at[i] = now
                self.push_progress_counts[i] = counts[i]
            elif self.clock.ticks_diff(now, self.push_progress_at[i]) >= 200:
                return True
        return False
