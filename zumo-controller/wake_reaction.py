"""Timed wake expression and six-LED sweep; no motor or blocking calls."""

DURATION_MS = 2200
FRAME_MS = 30
COLORS = ((0, 60, 255), (0, 230, 255), (155, 25, 255))


class WakeReaction:
    def __init__(self, face, leds, clock):
        self.face = face
        self.leds = leds
        self.clock = clock
        self.started = None
        self.last_frame = None
        self.saved = None

    @property
    def active(self):
        return self.started is not None

    def start(self, now):
        if self.active:
            return False
        if self.leds is None:
            raise ValueError("RGB LEDs unavailable")
        self.saved = [(tuple(self.leds.get(i)), self.leds.get_brightness(i))
                      for i in range(6)]
        self.started = now
        self.last_frame = None
        self.face.play("happy", hold_ms=1680)
        self.update(now)
        return True

    def cancel(self, reset_face=True):
        if not self.active:
            return
        saved = self.saved
        self.started = self.last_frame = self.saved = None
        if reset_face:
            self.face.play("neutral")
        for i, (color, brightness) in enumerate(saved):
            self.leds.set(i, color)
            self.leds.set_brightness(brightness, led=i)
        self.leds.show()

    def update(self, now):
        if not self.active:
            return
        elapsed = self.clock.ticks_diff(now, self.started)
        if elapsed >= DURATION_MS:
            self.cancel(reset_face=False)  # Face engine finishes its own transition.
            return
        if self.last_frame is not None and self.clock.ticks_diff(now, self.last_frame) < FRAME_MS:
            return
        self.last_frame = now
        for i in range(6):
            phase = max(0, elapsed - i * 90)
            color_index = (phase // 300) % len(COLORS)
            a, b = COLORS[color_index], COLORS[(color_index + 1) % len(COLORS)]
            blend = phase % 300
            color = tuple(a[c] + (b[c] - a[c]) * blend // 300 for c in range(3))
            pulse = abs((elapsed % 700) - 350)
            brightness = 5 + 7 * (350 - pulse) // 350
            if elapsed < i * 90:
                brightness = 0
            remaining = DURATION_MS - elapsed
            if remaining < 350:
                old_color, old_brightness = self.saved[i]
                color = tuple(old_color[c] + (color[c] - old_color[c]) * remaining // 350
                              for c in range(3))
                brightness = old_brightness + (brightness - old_brightness) * remaining // 350
            self.leds.set(i, color)
            self.leds.set_brightness(brightness, led=i)
        self.leds.show()
