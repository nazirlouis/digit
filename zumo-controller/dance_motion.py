"""One finite side-to-side dance. Hardware and safety stay in UARTController."""

SPEED = 3000
VISIBILITY_TIMEOUT_MS = 1500
DURATION_MS = 4800
MAX_DURATION_MS = 6000
STEPS = ((-1, 1, 80), (0, 0, 160), (1, -1, 160),
         (0, 0, 160), (-1, 1, 80), (0, 0, 160)) * 6


class DanceMotion:
    def __init__(self, clock):
        self.clock = clock
        self.cancel()

    def cancel(self):
        self.sequence = None
        self.requested = self.started = self.step_at = None
        self.index = 0
        self.result = None

    def start(self, sequence, now):
        self.cancel()
        self.sequence, self.requested = sequence, now

    def update(self, now, face):
        if self.result or self.sequence is None:
            return 0, 0
        diff = self.clock.ticks_diff
        if (face.active_scene_sequence != self.sequence or face.expression != "happy" or face.returning):
            self.result = "DANCE_FACE_CANCELLED"
            return 0, 0
        if self.started is None:
            if diff(now, self.requested) >= VISIBILITY_TIMEOUT_MS:
                self.result = "DANCE_FACE_TIMEOUT"
                return 0, 0
            if face.visible_scene_sequence != self.sequence:
                return 0, 0
            self.started = self.step_at = now
        if diff(now, self.started) >= MAX_DURATION_MS:
            self.result = "DANCE_TIMEOUT"
            return 0, 0
        if diff(now, self.step_at) >= STEPS[self.index][2]:
            self.index += 1
            self.step_at = now  # Never skip stopped pauses to catch up.
            if self.index == len(STEPS):
                self.result = "DANCE_DONE"
                return 0, 0
        left, right, _ = STEPS[self.index]
        return left * SPEED, right * SPEED


class DanceShow:
    COLORS = ((255, 125, 20), (40, 160, 255), (190, 45, 255), (40, 220, 90))

    def __init__(self, face, leds, clock):
        self.face, self.leds, self.clock = face, leds, clock
        self.started = self.saved = None
        self.frame = -1

    def update(self, now):
        if self.started is None:
            self.saved = [(tuple(self.leds.get(i)), self.leds.get_brightness(i)) for i in range(6)]
            self.started = now
        frame = self.clock.ticks_diff(now, self.started) // 100
        if frame == self.frame:
            return
        self.frame = frame
        for i in range(6):
            color = self.COLORS[(frame // 2 + i) % len(self.COLORS)] if frame % 2 == 0 else (0, 0, 0)
            self.leds.set(i, color)
            self.leds.set_brightness(12, led=i)
        self.leds.show()

    def cancel(self, reset_face=True):
        saved, active = self.saved, self.started is not None
        self.saved = self.started = None
        self.frame = -1
        if saved is not None:
            for i, (color, brightness) in enumerate(saved):
                try:
                    self.leds.set(i, color)
                    self.leds.set_brightness(brightness, led=i)
                except Exception:
                    pass
            try:
                self.leds.show()
            except Exception:
                pass
        if active and reset_face:
            try:
                self.face.play("neutral")
            except Exception:
                pass
