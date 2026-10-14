"""Bounded stationary recovery; command identity, leases and deadlines survive.

An interrupted push never resumes. Unobserved heading/control gaps require a
new plan even when sensor health returns. Nothing here clears a hard latch.
"""
import math

RECOVERY_MS = 2000
MAX_RECOVERIES = 2
GOOD_SAMPLES = 3


class LocalRecovery:
    def __init__(self, job):
        self.job = job
        self.active = False

    def begin(self, cause, now):
        j, c = self.job, self.job.c
        if self.active:
            return True
        if (not j.armed or not j.active or j.kind in ("EDGE", "BACK") or
                not c.ready or j.recovery_count >= MAX_RECOVERIES):
            return False
        self.kind, self.state = j.kind, j.state
        self.cause, self.started = cause, now
        self.good = 0
        self.range_at = self.gyro_at = None
        self.gyro_stable = False
        self.restart = 0
        self.resumable = (cause.startswith("RANGE_") and j.kind in ("TRAVEL", "DRIVE", "CIRCLE") and
                          j.state in ("RUNNING", "BRAKING") and j.bias is not None)
        self.terminal = ("OBSTACLE_STOPPED" if cause == "OBSTACLE" else
                         "PUSH_BLOCKED" if j.kind == "PUSH" else "BLOCKED_RECOVERED")
        c._stop()
        j.state, j.reason, j.settled = "RECOVERING", cause, None
        j.recovery_count += 1
        j.recovery_cause = cause
        if cause.startswith(("GYRO_", "LOOK_GYRO", "CONTROL_", "ENCODER_", "FEEDBACK_")) or cause == "TRACK_MISMATCH":
            j.position_known = False
        self.active = True
        j.snapshot(now=now)
        return True

    def _restart_range(self, now):
        # The ordinary driver start_ranging waits synchronously. Only call the
        # explicit nonblocking entry point, once and with tracks already off.
        c = self.job.c
        elapsed = c.clock.ticks_diff(now, self.started)
        if elapsed < 500 or self.restart >= 2 or c.tof is None:
            return
        try:
            if self.restart == 0 and hasattr(c.tof, "restart_ranging_nonblocking"):
                c.tof.stop_ranging()
                self.restart = 1
            elif self.restart == 1:
                c.tof.restart_ranging_nonblocking()
                self.restart = 2
        except Exception:
            self.restart = 2

    def tick(self, now):
        j, c = self.job, self.job.c
        elapsed = c.clock.ticks_diff(now, self.started)
        if elapsed >= RECOVERY_MS:
            self.active = False
            cause = self.cause
            if cause.startswith(("ENCODER_", "FEEDBACK_")):
                c._latch("ENCODER")
            else:
                j.cancel("RECOVERY_FAILED", True)
            return
        range_ok = c._range_valid(now) or c._range_no_return(now)
        if self.cause.startswith("RANGE_") and not range_ok:
            self.good = 0
            self._restart_range(now)
        try:
            gyro = c.voice_turn.gyro
            if gyro.data_ready():
                gyro.read()
                rate = gyro.last_reading_dps[2]
                if not math.isfinite(rate) or abs(rate) > 500:
                    raise ValueError("gyro")
                dt = c.clock.ticks_diff(now, j.gyro_at) if j.gyro_at is not None else 0
                if j.bias is not None and 0 <= dt <= 150:
                    j.heading += (rate - j.bias) * dt / 1000.0
                elif dt > 150:
                    self.resumable = False
                    j.position_known = False
                j.gyro_at = self.gyro_at = now
                self.gyro_stable = abs(rate - (j.bias or 0)) < 10
            if self.kind in ("TRAVEL", "PUSH"):
                j._sample_travel()
        except Exception:
            self.good = 0
            self.gyro_stable = False
            self.resumable = False
            return
        # A recovered stream must produce three distinct observations. A
        # stationary old sample cannot accumulate successful recovery ticks.
        fresh_range = range_ok and c.range_at != self.range_at
        if fresh_range:
            self.range_at = c.range_at
        gyro_ok = (self.gyro_stable and self.gyro_at is not None and
                   0 <= c.clock.ticks_diff(now, self.gyro_at) < 150)
        if not j._settled(now) or not range_ok or not gyro_ok:
            self.good = 0
            return
        if fresh_range:
            self.good += 1
        if self.good < GOOD_SAMPLES:
            return
        self.active = False
        if self.resumable:
            # Readiness recovered, but never resume into a newly observed wall.
            if (self.kind != "TRAVEL" or j.travel_target > 0) and c._range_valid(now) and c.distance_mm < 50:
                j._finish("OBSTACLE_STOPPED")
                return
            j.state, j.reason, j.settled = self.state, "RECOVERED", None
            j.last_range_at, j.last_distance = c.range_at, c.distance_mm
            j.closing_mm_s = 0.0
            j.closing_rates = [0.0, 0.0, 0.0]
            j.snapshot(now=now)
        else:
            if self.cause == "ENCODER_STALL" and self.kind != "PUSH":
                c._latch("STALL")  # No verified escape path was available.
            else:
                j._finish(self.terminal)
