"""Brief attentive face and quiet LED sweep; no motor or blocking calls."""

from reaction_catalog import WAKE_TIMING

DURATION_MS = sum(WAKE_TIMING)
VISIBILITY_TIMEOUT_MS = 1500
FRAME_MS = 30
COLORS = ((0, 100, 180), (0, 180, 200))


class WakeReaction:
    def __init__(self, face, leds, clock):
        self.face = face
        self.leds = leds
        self.clock = clock
        self.started = None
        self.last_frame = None
        self.saved = None
        self.scene_sequence = None
        self.waiting_visible = False

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
        if getattr(self.face, "cooperative", False):
            self.face.show_scene("happy", "NONE", timing=WAKE_TIMING)
            self.scene_sequence = self.face.scene_sequence
            self.waiting_visible = True
        else:
            self.face.play("happy", hold_ms=WAKE_TIMING[1])
            self.face.enter_ms, self.face.exit_ms = WAKE_TIMING[0], WAKE_TIMING[2]
        self.update(now)
        return True

    def cancel(self, reset_face=True):
        if not self.active:
            return
        saved = self.saved
        self.started = self.last_frame = self.saved = None
        self.scene_sequence = None
        self.waiting_visible = False
        if reset_face:
            self.face.play("neutral")
        for i, (color, brightness) in enumerate(saved):
            self.leds.set(i, color)
            self.leds.set_brightness(brightness, led=i)
        self.leds.show()

    def update(self, now):
        if not self.active:
            return
        if self.waiting_visible:
            if self.clock.ticks_diff(now, self.started) >= VISIBILITY_TIMEOUT_MS:
                self.cancel()
                return
            if getattr(self.face, "active_scene_sequence", self.scene_sequence) != self.scene_sequence:
                self.cancel(reset_face=False)
                return
            if self.face.visible_scene_sequence != self.scene_sequence:
                return
            self.waiting_visible = False
            self.started = getattr(self.face, "started", now)
        elapsed = self.clock.ticks_diff(now, self.started)
        if elapsed >= DURATION_MS:
            self.cancel(reset_face=False)  # Face engine finishes its own transition.
            return
        if self.last_frame is not None and self.clock.ticks_diff(now, self.last_frame) < FRAME_MS:
            return
        self.last_frame = now
        for i in range(6):
            phase = max(0, elapsed - i * 35)
            color_index = (phase // 300) % len(COLORS)
            a, b = COLORS[color_index], COLORS[(color_index + 1) % len(COLORS)]
            blend = phase % 300
            color = tuple(a[c] + (b[c] - a[c]) * blend // 300 for c in range(3))
            pulse = abs((elapsed % 500) - 250)
            brightness = 2 + 4 * (250 - pulse) // 250
            if elapsed < i * 35:
                brightness = 0
            remaining = DURATION_MS - elapsed
            if remaining < WAKE_TIMING[2]:
                old_color, old_brightness = self.saved[i]
                color = tuple(old_color[c] + (color[c] - old_color[c]) * remaining // WAKE_TIMING[2]
                              for c in range(3))
                brightness = old_brightness + (brightness - old_brightness) * remaining // WAKE_TIMING[2]
            self.leds.set(i, color)
            self.leds.set_brightness(brightness, led=i)
        self.leds.show()
