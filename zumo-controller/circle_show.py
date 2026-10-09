"""Happy faces and six-color light chase while a circle job is running."""

COLORS = ((255, 70, 20), (255, 180, 0), (30, 220, 90),
          (0, 180, 255), (150, 55, 255), (255, 45, 130))
FACES = ("happy", "excited", "happy", "love")
FRAME_MS = 180
FACE_MS = 1200


class CircleShow:
    def __init__(self, face, leds, clock):
        self.face, self.leds, self.clock = face, leds, clock
        self.started = None
        self.saved = None
        self.frame = self.face_index = -1
        self.failed = False

    def cancel(self):
        active, saved = self.started is not None, self.saved
        self.started = self.saved = None
        self.frame = self.face_index = -1
        self.failed = False
        if active and getattr(self.face, "cooperative", False):
            try:
                self.face.play("neutral")
            except Exception:
                pass
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

    def update(self, now):
        if self.failed:
            return
        try:
            if self.started is None:
                if self.leds is not None:
                    self.saved = [(tuple(self.leds.get(i)), self.leds.get_brightness(i)) for i in range(6)]
                self.started = now
            elapsed = self.clock.ticks_diff(now, self.started)
            face_index = elapsed // FACE_MS
            if face_index != self.face_index:
                self.face_index = face_index
                if getattr(self.face, "cooperative", False):
                    self.face.show_scene(FACES[face_index % len(FACES)], "NONE", timing=(180, 900, 120))
            frame = elapsed // FRAME_MS
            if frame != self.frame and self.leds is not None:
                self.frame = frame
                for i in range(6):
                    self.leds.set(i, COLORS[(i+frame) % len(COLORS)])
                    self.leds.set_brightness(12 if (i+frame) % 3 == 0 else 4, led=i)
                self.leds.show()
        except Exception as error:
            self.cancel()
            self.failed = True  # Decoration failure cannot flood the motor loop.
            print("CIRCLE SHOW:", error)
