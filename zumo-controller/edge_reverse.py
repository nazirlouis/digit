"""Straight encoder-bounded edge reverse; edge and IMU readings do not gate it."""
from edge_retreat import EdgeRetreat, REST_MS, SETTLE_LIMIT_MS

MAX_MM = 100
DRIVE_TARGET = 3000
LIMIT_MS = 10000
SETTLE_MS = REST_MS


class EdgeReverse:
    def __init__(self, controller):
        self.c = controller
        self.active = self.used = False
        self.state = "IDLE"
        self.manual = self.automatic = False
        self.back_mm = 0.0

    def available(self):
        c = self.c
        return (not self.used and c.fault == "EDGE" and c.edge_restore_thresholds is not None and
                c.calibration is None and c.speed_control is not None and
                hasattr(c.speed_control, "encoders") and c.motion.distance_scale is not None)

    def cancel(self):
        self.active = False

    def begin(self, now, distance=MAX_MM, manual=False, automatic=False):
        c = self.c
        if not self.available() or not 10 <= distance <= MAX_MM:
            return False
        try:
            self.origin = self.last_counts = c.speed_control.encoders.get_counts()
        except Exception:
            c._latch("ENCODER")
            return False
        self.thresholds = c.edge_restore_thresholds[:]
        self.mask = sum(1 << i for i in range(5) if c.last_line_readings[i] >= self.thresholds[i])
        self.used = self.active = True
        self.manual, self.automatic = manual, automatic
        self.state = "BACK"
        self.started = self.progress_at = now
        self.rest_at = self.settle_started = None
        self.back_mm = self.progress_mm = 0.0
        self.limit_mm = distance
        EdgeRetreat.start_direct(c, now)
        return True

    def begin_automatic(self, readings, now):
        c = self.c
        if (not c.ready or c.motion.armed or c.motion.active or not c._moving() or c.thresholds is None or
                c.speed_control is None or not hasattr(c.speed_control, "encoders") or c.motion.distance_scale is None):
            return False
        thresholds = c.thresholds[:]
        c._stop()
        c.edge_restore_thresholds = c.blocked_thresholds = thresholds
        c.thresholds = None
        c.ready, c.edge, c.fault = False, True, "EDGE"
        self.used = False
        if not self.begin(now, automatic=True):
            c.thresholds = thresholds if c.fault == "EDGE" else None
            return False
        c._send("0 EVENT EDGE_AUTO_START")
        return True

    def tick(self, now):
        if not self.active:
            return
        c = self.c
        try:
            if self.automatic and c.clock.ticks_diff(now, c.last_host_at) >= 750:
                raise RuntimeError("EDGE_HOST_TIMEOUT")
            done = EdgeRetreat.direct_step(self, c, now)
            self.active = True  # Output cutoff keeps the local completion owner alive.
            if done:
                c.edge_release_mask = 31
                if not c._restore_edge_ready(now):
                    raise RuntimeError("EDGE_FEEDBACK")
        except Exception as error:
            reason = str(error)
            c.edge_retreat.diagnose(reason, now, self.state, self.back_mm)
            c._latch(reason if reason.startswith(("EDGE_", "ENCODER_", "BUTTON_")) else "EDGE_FEEDBACK")
