"""Finite expression gestures. The UART controller owns safety and motor I/O.

Power stays at the user's measured breakaway floor. Short pulses and stopped
pauses set the size/tempo, independently of the manual acceleration slider.
Equal opposite pulse times aim to restore heading; they are not measured turns.
"""

SPEED = 3000
VISIBILITY_TIMEOUT_MS = 1500
LINK_TIMEOUT_MS = 500
MAX_LOOP_GAP_MS = 50
MAX_DURATION_MS = 2000


def wiggle(pulse, pause):
    return ((-1, 1, pulse), (0, 0, pause),
            (1, -1, pulse * 2), (0, 0, pause),
            (-1, 1, pulse), (0, 0, pause))


# (left direction, right direction, milliseconds). Every reversal has a stop.
GESTURES = {
    "neutral": (),
    "happy": wiggle(60, 100),
    "sad": ((-1, -1, 75), (0, 0, 500)),
    "mad": wiggle(35, 90) * 2,
    "serious": (),
    "curious": ((-1, 1, 70), (0, 0, 380), (1, -1, 70), (0, 0, 160)),
    "surprised": ((-1, -1, 100), (0, 0, 420)),
    "sleeping": (),
    "confused": ((-1, 1, 60), (0, 0, 220), (1, -1, 120),
                 (0, 0, 380), (-1, 1, 60), (0, 0, 150)),
    "sleepy": ((-1, 1, 45), (0, 0, 600), (1, -1, 45), (0, 0, 150)),
    "excited": wiggle(65, 70) * 2,
    "crying": wiggle(25, 100) * 3,
    "love": ((-1, 1, 60), (0, 0, 320), (1, -1, 120),
             (0, 0, 320), (-1, 1, 60), (0, 0, 240)),
}


class ExpressionMotion:
    def __init__(self, clock):
        self.clock = clock
        self.cancel()

    @property
    def active(self):
        return bool(self.steps)

    def cancel(self):
        self.completed = False
        self.steps = ()
        self.sequence = None
        self.started = self.step_at = None
        self.index = 0

    def start(self, expression, sequence, now, require_link=True):
        self.cancel()
        self.steps = GESTURES.get(expression, ())
        self.sequence = sequence
        self.requested = now
        self.require_link = require_link

    def update(self, now, face):
        if not self.active:
            return (0, 0)
        diff = self.clock.ticks_diff
        # Replacement, automatic neutral, and early scene expiry never leave
        # a pending gesture that could start when the robot becomes idle later.
        if (face.active_scene_sequence != self.sequence or
                face.expression == "neutral" or face.returning):
            self.cancel()
            return (0, 0)
        if self.started is None:
            if diff(now, self.requested) >= VISIBILITY_TIMEOUT_MS:
                self.cancel()
                return (0, 0)
            if face.visible_scene_sequence != self.sequence:
                return (0, 0)
            self.started = self.step_at = now
        if diff(now, self.started) >= MAX_DURATION_MS:
            self.cancel()
            return (0, 0)
        if diff(now, self.step_at) >= self.steps[self.index][2]:
            self.index += 1
            self.step_at = now  # Do not skip the stopped pause to catch up.
            if self.index == len(self.steps):
                self.cancel()
                self.completed = True
                return (0, 0)
        left, right, _ = self.steps[self.index]
        return (left * SPEED, right * SPEED)
