"""Automatic 100 mm edge reverse; the interrupted job never resumes.

The separate obstruction backoff retains its 20 mm straight-path bounds.
Both retain the original command identity, heartbeat and deadline.
"""
import math

MAX_BACK_MM = 20
EDGE_BACK_MM = 100
EDGE_LIMIT_MS = 10000
SETTLE_LIMIT_MS = 2500
MIN_PATH_MM = 35
CLEAR_MARGIN = 30
CLEAR_BACK_MM = 5
FRESH_MS = 100
REST_MS = 200
LIMIT_MS = 2500
WAIT_MS = 800
SPEED_MM_S = 25


class EdgeRetreat:
    def __init__(self, controller, accelerometer):
        self.c = controller
        self.acc = accelerometer
        self.allowed = False
        self.mask = 0
        self.back_mm = 0.0
        self.forward_mm = 0.0
        self.acc_at = self.gyro_at = None
        self.level = self.steady = False
        self.clean_path = 0.0
        self.line_at = None
        self.blocked = False
        self.reject_reason = self.reject_detail = ""
        self.diagnostic_at = None

    @property
    def active(self):
        return self.c.motion.kind == "EDGE"

    def prepare(self, allowed):
        self.reject_reason = self.reject_detail = ""
        self.diagnostic_at = None
        self.allowed = allowed
        self.mask = 0
        self.back_mm = self.forward_mm = self.clean_path = 0.0
        self.acc_at = self.gyro_at = self.line_at = None
        self.level = self.steady = False
        self.blocked = False

    def sample_imu(self, now, rates):
        self.gyro_at = now
        self.steady = (len(rates) == 3 and
                       all(math.isfinite(v) and abs(v) <= 8 for v in rates))
        self.c._sample_accelerometer(now)
        self.acc_at = self.c.accel_at
        self.level = self.ramp_safe(self.c.accel_reading)

    @staticmethod
    def ramp_safe(values):
        if values is None:
            return False
        x, y, z = values
        norm = x*x+y*y+z*z
        # A steady ramp up to 60 degrees is eligible for cautious retreat.
        return .70 <= norm <= 1.30 and z >= .5 * math.sqrt(norm)

    def stable(self, now):
        clock = self.c.clock
        return (self.level and self.steady and self.acc_at is not None and
                self.gyro_at is not None and
                0 <= clock.ticks_diff(now, self.acc_at) <= FRESH_MS and
                0 <= clock.ticks_diff(now, self.gyro_at) <= FRESH_MS)

    def line_sample(self, readings, now):
        c, job = self.c, self.c.motion
        self.line_at = now
        mask = sum(1 << i for i in range(5) if readings[i] >= c.thresholds[i])
        if not self.active:
            if (not mask and self.allowed and job.kind in ("TRAVEL", "PUSH") and
                    job.state == "RUNNING" and job.travel_target > 0):
                self.clean_path = min(job.travel_tracks)
            return
        if not self.blocked:
            return  # A detected edge commits to the encoder-bounded reverse.
        # A newly affected sensor, including a previously cleared sensor, means
        # the retreat is not predictably taking us away from the original edge.
        if mask & ~self.remaining_mask:
            job.cancel("EDGE_WORSENED", True)
            return
        self.remaining_mask = mask
        clear = all(readings[i] < c.thresholds[i]-CLEAR_MARGIN for i in range(5))
        if clear:
            if self.clear_at is None:
                self.clear_at, self.clear_back = now, None
        else:
            self.clear_at = None
            if job.reason in ("EDGE_SETTLE", "BLOCK_SETTLE"):
                job.cancel("EDGE_NOT_CLEAR", True)

    def begin(self, readings, now, blocked=False):
        c, job = self.c, self.c.motion
        reason = self.rejection(now, blocked) if blocked else self.edge_rejection(now)
        c._stop()  # Stop before any encoder I/O or reporting, even if ineligible.
        self.allowed = False
        if reason:
            return self.reject(reason, readings, now)
        try:
            counts = c.speed_control.encoders.get_counts()
            tracks = [job.travel_tracks[i] + self.delta(counts[i], job.travel_counts[i]) /
                      job.distance_scale[i] for i in range(2)] if job.kind in ("TRAVEL", "PUSH") else [job.travelled_mm]*2
            if blocked and (min(tracks) < MIN_PATH_MM or abs(tracks[0]-tracks[1]) > 5):
                return self.reject("PATH_CHANGED", readings, now)
        except Exception:
            return self.reject("ENCODER_IO", readings, now)
        self.origin = self.last_counts = counts
        self.forward_mm = sum(tracks)/2
        self.heading = job.heading
        self.blocked = blocked
        self.started = now
        self.rest_at = self.clear_at = None
        self.settle_started = None
        self.state = "BACK"
        self.limit_mm = EDGE_BACK_MM
        self.clear_back = self.back_mm = 0.0
        self.progress_at = now
        self.progress_mm = 0.0
        self.mask = sum(1 << i for i in range(5) if readings[i] >= c.thresholds[i])
        self.remaining_mask = self.mask
        self.line_at = now
        if not blocked:
            c.ready, c.edge, c.fault = False, True, "EDGE_RETREAT"
        # Keep the original job identity, lease and deadline; old job retries
        # return this state. No new ordinary job can run until this one ends.
        job.kind, job.state, job.reason = "EDGE", "RECOVERING", "BLOCK_WAIT" if blocked else "EDGE_BACK"
        job.recovery.active = False
        job.gyro_at = now
        if job.bias is None:
            job.bias = 0.0
        job.recovery_count += 1
        job.recovery_cause = "ENCODER_STALL" if blocked else "EDGE"
        job.travelled_mm = self.forward_mm
        if not blocked:
            self.start_direct(c, now)
        job.snapshot(now=now)
        return True

    def edge_rejection(self, now):
        c, job = self.c, self.c.motion
        if not job.armed or not job.active: return "NOT_ARMED"
        if job.kind in ("EDGE", "BACK", "MEASURE"): return "JOB_TYPE"
        if not c.ready or c.fault != "NONE": return "NOT_READY"
        if c.speed_control is None or not hasattr(c.speed_control, "encoders"): return "NO_ENCODERS"
        if job.distance_scale is None: return "NO_CALIBRATION"
        if not 0 <= c.clock.ticks_diff(now, c.speed_control.at) <= FRESH_MS: return "ENCODER_STALE"
        if c.clock.ticks_diff(now, job.heartbeat) >= 750: return "HEARTBEAT"
        if c.clock.ticks_diff(now, job.started) >= job.limit: return "DEADLINE"
        if c.button_c.is_pressed(): return "BUTTON_C"
        return ""

    def rejection(self, now, blocked):
        c, job = self.c, self.c.motion
        self.acc_at = c.accel_at
        self.level = self.ramp_safe(c.accel_reading)
        if not self.allowed: return "NO_PERMISSION"
        if self.acc is None: return "ACC_MISSING"
        if not job.armed: return "NOT_ARMED"
        if job.kind not in ("TRAVEL", "PUSH"): return "JOB_TYPE"
        if job.state != "RUNNING": return "JOB_STATE"
        if job.travel_target <= 0: return "NOT_FORWARD"
        if not c.ready or c.fault != "NONE": return "NOT_READY"
        if c.left <= 0 or c.right <= 0: return "OUTPUT_DIRECTION"
        if self.clean_path < MIN_PATH_MM: return "PATH_SHORT"
        if abs(job.heading-job.target_heading) > 5: return "HEADING_ERROR"
        if abs(job.travel_tracks[0]-job.travel_tracks[1]) > 5: return "TRACK_MISMATCH"
        if job.distance_scale is None: return "NO_CALIBRATION"
        if c.accel_error != "NONE": return c.accel_error
        if self.acc_at is None or not 0 <= c.clock.ticks_diff(now, self.acc_at) <= FRESH_MS: return "ACC_STALE"
        if not self.level: return "ACC_TILT_OR_FORCE"
        if self.gyro_at is None or not 0 <= c.clock.ticks_diff(now, self.gyro_at) <= FRESH_MS: return "GYRO_STALE"
        if not self.steady: return "ROTATING"
        if not 0 <= c.clock.ticks_diff(now, c.speed_control.at) <= FRESH_MS: return "ENCODER_STALE"
        if not blocked and not all(v > 50 for v in c.speed_control.velocity): return "NOT_MOVING"
        if c.clock.ticks_diff(now, job.heartbeat) >= 750: return "HEARTBEAT"
        if c.clock.ticks_diff(now, job.started) >= job.limit: return "DEADLINE"
        if c.button_c.is_pressed(): return "BUTTON_C"
        return ""

    def reject(self, reason, readings, now):
        # Only allocate diagnostics after motors are off, once per rejected edge.
        c, job = self.c, self.c.motion
        self.reject_reason = reason
        ages = [min(65535, max(0, c.clock.ticks_diff(now, at))) if at is not None else -1
                for at in (self.acc_at, self.gyro_at, c.speed_control.at if c.speed_control else None)]
        acc = c.accel_reading
        self.reject_detail = "floor:{};limit:{};acc_mg:{};age_ms:{};path_mm:{};heading_ddeg:{};track_mm:{};state:{}".format(
            ",".join(str(v) for v in readings),
            ",".join(str(v) for v in (c.thresholds or ())),
            ",".join(str(int(max(-32, min(32, v))*1000)) for v in acc) if acc else "missing",
            ",".join(str(v) for v in ages), int(self.clean_path),
            int((job.heading-job.target_heading)*10),
            int(job.travel_tracks[0]-job.travel_tracks[1]), job.state)[:255]
        return False

    def diagnose(self, reason, now, phase=None, back_mm=None):
        c, job = self.c, self.c.motion
        self.reject_reason = reason
        if self.diagnostic_at == now:
            return
        self.diagnostic_at = now
        def axes(values, scale):
            if values is None or len(values) != 3 or not all(math.isfinite(v) for v in values):
                return "invalid"
            return ",".join(str(int(max(-1000, min(1000, v))*scale)) for v in values)
        gyro = c.voice_turn.gyro if c.voice_turn else None
        ages = [max(0, min(65535, c.clock.ticks_diff(now, at))) if at is not None else -1
                for at in (self.acc_at, self.gyro_at, c.speed_control.at if c.speed_control else None)]
        self.reject_detail = "phase:{};acc_mg:{};gyro_mdps:{};age_ms:{};floor:{};cps:{};output:{};back_mm:{}".format(
            phase or job.reason, axes(c.accel_reading, 1000), axes(getattr(gyro, "last_reading_dps", None), 1000),
            ",".join(str(v) for v in ages), ",".join(str(v) for v in (c.last_line_readings or ())),
            ",".join(str(int(v)) for v in c.speed_control.velocity) if c.speed_control else "missing",
            "{},{}".format(c.left, c.right), int(self.back_mm if back_mm is None else back_mm))[:255]

    @staticmethod
    def start_direct(c, now):
        # Prime the normal 3000-minimum encoder controller without its slow
        # startup ramp. Output is applied only after queued STOP is processed.
        c._target_motion(-3000, -3000, now, 6000)
        c.ramp_left = c.ramp_right = -3000.0

    @staticmethod
    def direct_step(owner, c, now):
        clock = c.clock
        if c.button_c.is_pressed():
            raise RuntimeError("BUTTON_C")
        if clock.ticks_diff(now, owner.started) >= EDGE_LIMIT_MS:
            raise RuntimeError("EDGE_TIMEOUT")
        if clock.ticks_diff(now, c.speed_control.at) > FRESH_MS:
            raise RuntimeError("ENCODER_STALE")
        counts = c.speed_control.encoders.get_counts()
        for i in range(2):
            EdgeRetreat.delta(counts[i], owner.last_counts[i])
        scales = c.motion.distance_scale
        tracks = [-EdgeRetreat.delta(counts[i], owner.origin[i])/scales[i] for i in range(2)]
        owner.last_counts = counts
        owner.back_mm = sum(tracks)/2
        if owner.state == "SETTLE":
            # Further floor/IMU readings cannot cancel the committed reverse or
            # its completion. Actual stopped encoders still precede replanning.
            if clock.ticks_diff(now, owner.settle_started) >= SETTLE_LIMIT_MS:
                raise RuntimeError("EDGE_NOT_STOPPED")
            if not c.motion._stopped(now):
                owner.rest_at = None
            elif owner.rest_at is None:
                owner.rest_at = now
            elif clock.ticks_diff(now, owner.rest_at) >= REST_MS:
                if not owner.limit_mm-2 <= owner.back_mm <= owner.limit_mm+10:
                    raise RuntimeError("EDGE_DISTANCE")
                return True
            return False
        # Cut power at the distance target. Sampling/coast is measured and
        # reported, rather than falsely reporting exactly 100 mm displacement.
        if owner.back_mm >= owner.limit_mm or max(tracks) >= owner.limit_mm+2:
            c._stop()
            owner.state, owner.settle_started, owner.rest_at = "SETTLE", now, None
            return False
        if owner.back_mm >= owner.progress_mm+1:
            owner.progress_at, owner.progress_mm = now, owner.back_mm
        elif clock.ticks_diff(now, owner.progress_at) >= 1000:
            raise RuntimeError("EDGE_STALL")
        c._target_motion(-3000, -3000, now, 6000)
        return False

    @staticmethod
    def delta(value, previous):
        delta = ((value-previous+0x80000000) & 0xffffffff)-0x80000000
        if abs(delta) > 10000:
            raise RuntimeError("ENCODER_RANGE")
        return delta

    def lock(self, fault=False):
        c = self.c
        self.allowed = False
        if self.blocked:
            if fault:
                c.thresholds = None
                c.ready, c.edge = False, False
                if c.fault in ("NONE", "RECOVERING"):
                    c.fault = "RECOVERY_FAILED"
            return
        c.blocked_thresholds = c.thresholds
        c.thresholds = None
        c.ready = False
        if c.fault in ("NONE", "EDGE", "EDGE_RETREAT"):
            c.edge, c.fault = True, "EDGE"

    def tick(self, now):
        c, job, clock = self.c, self.c.motion, self.c.clock
        if not self.blocked:
            try:
                previous = self.state
                done = self.direct_step(self, c, now)
                job.travelled_mm = self.forward_mm-self.back_mm
                if done:
                    c.ready, c.edge, c.fault = True, False, "NONE"
                    c.edge_since = [None]*5
                    c.edge_release_mask = 31
                    c.blocked_thresholds = None
                    job._finish("EDGE_RETREATED")
                elif previous != self.state:
                    job.reason = "EDGE_SETTLE"
                    job.snapshot(now=now)
            except Exception as error:
                reason = str(error)
                self.diagnose(reason, now)
                job.cancel(reason if reason.startswith(("EDGE_", "ENCODER_", "BUTTON_")) else "EDGE_FEEDBACK", True)
            return
        # Obstruction backoff is separate and retains its strict 20 mm policy.
        try:
            job._gyro(now)
            c._sample_accelerometer(now)
            if c.accel_error != "NONE":
                raise RuntimeError("EDGE_IMU_INVALID")
            self.acc_at, self.level = c.accel_at, self.ramp_safe(c.accel_reading)
            waiting = job.reason == "BLOCK_WAIT"
            stable = self.stable(now)
            if not stable and not waiting:
                raise RuntimeError("EDGE_UNSTABLE")
            if (self.line_at is None or clock.ticks_diff(now, self.line_at) > FRESH_MS or
                    clock.ticks_diff(now, c.speed_control.at) > FRESH_MS):
                raise RuntimeError("EDGE_STALE")
            if not (c._range_valid(now) or c._range_no_return(now)):
                raise RuntimeError("RANGE_UNAVAILABLE")
            counts = c.speed_control.encoders.get_counts()
            for i in range(2):
                self.delta(counts[i], self.last_counts[i])
            tracks = [-self.delta(counts[i], self.origin[i])/job.distance_scale[i] for i in range(2)]
            self.last_counts = counts
            self.back_mm = sum(tracks)/2
            if self.clear_at is not None and self.clear_back is None:
                self.clear_back = self.back_mm
            job.travelled_mm = self.forward_mm-self.back_mm
            if min(tracks) < -2 or abs(tracks[0]-tracks[1]) > 3:
                raise RuntimeError("EDGE_TRACKS")
            if abs(job.heading-self.heading) > 5:
                raise RuntimeError("EDGE_HEADING")
            if max(tracks) > MAX_BACK_MM:
                raise RuntimeError("EDGE_DISTANCE")
            if clock.ticks_diff(now, self.started) >= LIMIT_MS:
                raise RuntimeError("EDGE_TIMEOUT")
            if waiting:
                if not stable:
                    self.rest_at = None
                    if clock.ticks_diff(now, self.started) >= WAIT_MS:
                        raise RuntimeError("EDGE_UNSTABLE")
                    return
                if clock.ticks_diff(now, self.started) >= WAIT_MS:
                    raise RuntimeError("EDGE_NOT_STOPPED")
                if not job._stopped(now):
                    self.rest_at = None
                elif self.rest_at is None:
                    self.rest_at = now
                elif clock.ticks_diff(now, self.rest_at) >= REST_MS:
                    job.reason, self.progress_at = "BLOCK_BACK", now
                    job.snapshot(now=now)
                return
            if job.reason == "BLOCK_SETTLE":
                if self.clear_at is None:
                    raise RuntimeError("EDGE_NOT_CLEAR")
                if job._settled(now) and clock.ticks_diff(now, self.clear_at) >= REST_MS:
                    c.ready, c.edge, c.fault = True, False, "NONE"
                    c.edge_since = [None]*5
                    c.blocked_thresholds = None
                    job._finish("BLOCKED_RECOVERED")
                return
            if self.back_mm >= self.progress_mm+1:
                self.progress_at, self.progress_mm = now, self.back_mm
            elif clock.ticks_diff(now, self.progress_at) >= (1000 if self.progress_mm < 1 else 500):
                raise RuntimeError("EDGE_STALL")
            if any(abs(c.speed_control.velocity[i])/job.distance_scale[i] > 50 for i in range(2)):
                raise RuntimeError("EDGE_SPEED")
            clear_back = self.clear_at is not None and self.back_mm-self.clear_back >= 10
            if clear_back or max(tracks) >= MAX_BACK_MM-2:
                c._stop()
                if not clear_back:
                    raise RuntimeError("EDGE_NOT_CLEAR")
                job.reason, job.settled = "BLOCK_SETTLE", None
                job.snapshot(now=now)
                return
            ratios = tuple(min(.5, SPEED_MM_S*job.distance_scale[i] /
                               abs(c.speed_control.rate_target(-3000))) for i in range(2))
            if any(not math.isfinite(v) or v <= 0 for v in ratios):
                raise RuntimeError("EDGE_SPEED")
            c._target_motion(-3000, -3000, now, 6000, rate_ratios=ratios)
        except Exception as error:
            reason = str(error)
            self.diagnose(reason, now)
            job.cancel(reason if reason.startswith(("EDGE_", "GYRO_", "ENCODER_", "RANGE_")) else "EDGE_FEEDBACK", True)
