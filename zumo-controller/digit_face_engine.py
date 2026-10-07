"""Shared eyes-only animation, with no hardware imports or blocking waits.

Poses use the LCD's 136x112 design coordinates; the OLED scales them to fit.
Each eye is (dx, dy, width, height, outer_lid, inner_lid, curve, heart, tilt, question).
Lids remove the whole area above their edge, never a stripe through an eye.
"""

import math
import random
import time


EXPRESSIONS = (
    "neutral", "happy", "sad", "mad", "serious", "curious", "surprised",
    "sleeping", "confused", "sleepy", "excited", "crying", "love",
)

# RGB565 eye colors. The background stays black; OLED uses the same shapes
# in monochrome. Edit this palette to tune the LCD without changing poses.
COLORS = {
    "neutral": 0xFCE0,    # Digit orange
    "happy": 0xFE20,      # Warm gold
    "sad": 0x349F,        # Soft blue
    "mad": 0xF800,        # Red
    "serious": 0xC638,    # Silver
    "curious": 0x07D5,    # Turquoise
    "surprised": 0x07FF,  # Cyan after the opening flash
    "sleeping": 0x292B,   # Dim indigo
    "confused": 0xB31F,   # Violet
    "sleepy": 0x72F5,     # Muted lavender
    "excited": 0xFFE0,    # Bright yellow
    "crying": 0x22BF,     # Deeper blue
    "love": 0xFB56,       # Pink
}
SURPRISE_FLASH_COLOR = 0xFFFF  # One white eye flash; never the background.
SURPRISE_FLASH_MS = 160

OPEN = (0, 0, 48, 70, 0, 0, 0, 0, 0, 0)
NEUTRAL = (OPEN, OPEN)
# (two eye poses, enter ms, hold ms, exit ms). None holds until replaced.
STYLES = {
    "neutral": (NEUTRAL, 240, None, 0),
    "happy": (((0, 7, 48, 7, 0, 0, -18, 0, 0),) * 2, 240, 2400, 280),
    "sad": (((0, 7, 46, 58, 28, 8, 0, 0, 0),) * 2, 420, 3400, 480),
    "mad": (((0, 2, 48, 54, 7, 30, 0, 0, 0),) * 2, 190, 2600, 320),
    "serious": (((0, 1, 48, 46, 17, 17, 0, 0, 0),) * 2, 250, 3000, 320),
    "curious": (((4, -5, 47, 74, 0, 0, 0, 0, 0),
                 (4, 3, 45, 54, 0, 0, 0, 0, 0)), 300, 2800, 320),
    "surprised": (((0, -4, 52, 84, 0, 0, 0, 0, 0),) * 2, 130, 1500, 380),
    "sleeping": (((0, 7, 44, 6, 0, 0, 8, 0, 0),) * 2, 850, None, 400),
    "confused": (((0, -3, 44, 68, 0, 0, 0, 0, 0, 1024),
                  (0, 3, 44, 68, 0, 0, 0, 0, 0, 1024)), 320, 3200, 360),
    "sleepy": (((0, 5, 46, 58, 31, 31, 0, 0, 0),) * 2, 650, 4400, 500),
    "excited": (((0, -4, 50, 78, 0, 0, 0, 0, 0),) * 2, 160, 2700, 300),
    "crying": (((0, 7, 46, 58, 28, 8, 0, 0, 0),) * 2, 420, 3800, 480),
    "love": (((0, 0, 48, 48, 0, 0, 0, 1024, 0),) * 2, 340, 3000, 380),
}
# Keep the question-mark morph channel in every pose, including interruptions.
STYLES = {name: (tuple(eye if len(eye) == 10 else eye + (0,) for eye in style[0]),) + style[1:]
          for name, style in STYLES.items()}


class FaceEngine:
    EXPRESSIONS = EXPRESSIONS
    CANVAS_WIDTH = 136
    CANVAS_HEIGHT = 112
    EYE_WIDTH = 48
    EYE_HEIGHT = 70
    CORNER_RADIUS = 24
    FRAME_MS = 30
    BLINK_DURATION_MS = 110
    BLINK_MIN_HEIGHT = 6
    MOVE_PROMPT_MS = 2000
    MOVE_FONT = {
        "m": ("00000", "11010", "10101", "10101", "10101", "10101", "10101"),
        "o": ("00000", "01110", "10001", "10001", "10001", "10001", "01110"),
        "v": ("00000", "10001", "10001", "10001", "01010", "01010", "00100"),
        "e": ("00000", "01110", "10001", "11111", "10000", "10001", "01110"),
        "?": ("01110", "10001", "00001", "00010", "00100", "00000", "00100"),
    }
    LOOK_POSITIONS = ((-7, 0), (7, 0), (0, -5), (0, 5),
                      (-5, -3), (5, -3), (-4, 3), (4, 3), (0, 0))

    def __init__(self, display):
        self.display = display
        self.color = None
        self._set_display_colors = getattr(display, "set_colors", None)
        self.expression = "neutral"
        self.pose = NEUTRAL
        self.from_pose = NEUTRAL
        self.return_pose = NEUTRAL
        self.started = time.ticks_ms()
        self.enter_ms = 0
        self.hold_ms = None
        self.exit_ms = 0
        self.returning = False
        self.last_frame = None
        self.blink_started = None
        self.look_started = None
        self.look_from = (0, 0)
        self.look_to = (0, 0)
        self.look = (0, 0)
        self.move_prompt_until = None
        self.caption = ""
        self.caption_until = None
        self.caption_dirty = False
        self._schedule_idle(self.started)
        self._set_color(COLORS["neutral"])
        self._draw(self.pose)

    def _set_color(self, color):
        if color == self.color:
            return False
        if self._set_display_colors is not None:
            # The LCD updates its pixel lookup and invalidates the previous
            # frame. Do this only at palette changes, not every animation tick.
            self._set_display_colors(color, 0x0000)
        self.color = color
        return True

    def _expression_color(self, elapsed):
        if self.expression == "surprised" and elapsed < SURPRISE_FLASH_MS:
            return SURPRISE_FLASH_COLOR
        return COLORS[self.expression]

    @staticmethod
    def _ease(elapsed, duration):
        if duration <= 0 or elapsed >= duration:
            return 1024
        if elapsed <= 0:
            return 0
        value = elapsed * 1024 // duration
        return value * value * (3072 - 2 * value) // (1024 * 1024)

    @staticmethod
    def _mix(start, target, amount):
        return tuple(tuple(a + (b - a) * amount // 1024
                           for a, b in zip(left, right))
                     for left, right in zip(start, target))

    @staticmethod
    def _wave(elapsed, period):
        """Small, smooth breathing/bob cycle, starting at its resting height."""
        return int(math.sin(2 * math.pi * (elapsed % period) / period) * 1024)

    @staticmethod
    def _offset(pose, dx=0, dy=0):
        return tuple((eye[0] + dx, eye[1] + dy) + eye[2:] for eye in pose)

    def _schedule_idle(self, now):
        self.next_blink = time.ticks_add(now, random.randint(2600, 5600))
        self.next_look = time.ticks_add(now, random.randint(1200, 3200))

    def _blink_height(self, elapsed):
        if elapsed <= 0 or elapsed >= self.BLINK_DURATION_MS:
            return self.EYE_HEIGHT
        half = self.BLINK_DURATION_MS // 2
        closure = (elapsed * 1024 // half if elapsed <= half else
                   (self.BLINK_DURATION_MS - elapsed) * 1024 //
                   (self.BLINK_DURATION_MS - half))
        return self.EYE_HEIGHT - (self.EYE_HEIGHT - self.BLINK_MIN_HEIGHT) * closure // 1024

    def play(self, expression, hold_ms=None):
        """Start from the displayed pose. A negative hold persists until replaced."""
        name = expression.lower()
        if name not in STYLES:
            raise ValueError("unknown face: " + expression)
        _, enter, hold, exit_ms = STYLES[name]
        self.caption_dirty = bool(self.caption)
        self.caption = ""
        self.caption_until = None
        self.from_pose = self.pose
        self.expression = name
        self.started = time.ticks_ms()
        self.enter_ms = enter
        self.hold_ms = hold if hold_ms is None else (None if hold_ms < 0 else hold_ms)
        self.exit_ms = exit_ms
        self.returning = False
        self.move_prompt_until = None
        self.blink_started = None
        self.look_started = None
        self.look = (0, 0)
        self.last_frame = None
        self._schedule_idle(self.started)

    def play_happy(self):
        self.play("happy")

    def play_sad(self):
        self.play("sad")

    def show_happy(self):
        # Retained for the spin demo's immediate happy introduction.
        self.play("happy", hold_ms=-1)
        self.enter_ms = 0
        self.pose = STYLES["happy"][0]
        self._set_color(COLORS["happy"])
        self._draw(self.pose)

    def show_scene(self, expression, caption):
        """Render a complete stationary action pose and a short readable label."""
        self.play(expression, hold_ms=-1)
        self.enter_ms = 0
        self.pose = STYLES[expression][0]
        self.caption = caption.replace("_", " ") if caption != "NONE" else ""
        self.caption_until = time.ticks_add(time.ticks_ms(), 8000)
        self._set_color(COLORS[expression])
        self._draw(self.pose)

    def show_move_prompt(self):
        self.play("neutral")
        self._set_color(COLORS["neutral"])
        self.move_prompt_until = time.ticks_add(time.ticks_ms(), self.MOVE_PROMPT_MS)
        self.display.fill(0)
        x0, y0 = (self.CANVAS_WIDTH - 116) // 2, (self.CANVAS_HEIGHT - 28) // 2
        for index, character in enumerate("move?"):
            for row, pixels in enumerate(self.MOVE_FONT[character]):
                for column, pixel in enumerate(pixels):
                    if pixel == "1":
                        self.display.fill_rect(x0 + index * 24 + column * 4,
                                               y0 + row * 4, 4, 4, 1)
        self.display.show()
        self.last_draw_prompt = True

    def _hold_pose(self, elapsed):
        name = self.expression
        pose = STYLES[name][0]
        if name == "happy":
            return self._offset(pose, dy=self._wave(elapsed, 1600) * 2 // 1024)
        if name == "excited":
            # Wide-eyed anticipation, then a smile, once per gentle bounce.
            phase = elapsed % 1200
            smile = STYLES["happy"][0]
            if phase < 400:
                amount = self._ease(phase, 400)
            elif phase < 800:
                amount = 1024
            else:
                amount = 1024 - self._ease(phase - 800, 400)
            return self._offset(self._mix(pose, smile, amount),
                                dy=self._wave(elapsed, 1200) * 3 // 1024)
        if name == "sleeping":
            return self._offset(pose, dy=self._wave(elapsed, 3200) * 2 // 1024)
        if name == "sleepy":
            phase = elapsed % 3200
            closed = STYLES["sleeping"][0]
            if phase < 1900:
                amount = self._ease(phase, 1900)
            elif phase < 2300:
                amount = 1024
            else:
                amount = 1024 - self._ease(phase - 2300, 900)
            return self._mix(pose, closed, amount)
        if name == "curious":
            return self._offset(pose, dx=self._wave(elapsed, 3600) * 3 // 1024)
        if name == "confused":
            pose = self._offset(pose, dx=self._wave(elapsed, 2200) * 4 // 1024)
            return pose  # Keep both punctuation dots readable throughout the hold.
        if name == "love":
            pulse = self._wave(elapsed, 1800) * 2 // 1024
            return tuple(eye[:2] + (eye[2] + pulse, eye[3] + pulse) + eye[4:]
                         for eye in pose)
        return pose

    def _close(self, pose, height):
        amount = (self.EYE_HEIGHT - height) * 1024 // (self.EYE_HEIGHT - self.BLINK_MIN_HEIGHT)
        closed = tuple(eye[:2] + (eye[2], self.BLINK_MIN_HEIGHT, 0, 0, 0, 0, 0, 0)
                       for eye in pose)
        return self._mix(pose, closed, amount)

    def _idle_pose(self, now):
        if self.look_started is not None:
            elapsed = time.ticks_diff(now, self.look_started)
            amount = self._ease(elapsed, 220)
            self.look = tuple(a + (b - a) * amount // 1024
                              for a, b in zip(self.look_from, self.look_to))
            if elapsed >= 220:
                self.look_started = None
                self.next_look = time.ticks_add(now, random.randint(1200, 3200))
        elif time.ticks_diff(now, self.next_look) >= 0:
            self.look_from = self.look
            self.look_to = self.LOOK_POSITIONS[random.randint(0, len(self.LOOK_POSITIONS) - 1)]
            self.look_started = now
        pose = self._offset(NEUTRAL, *self.look)
        if self.blink_started is None and time.ticks_diff(now, self.next_blink) >= 0:
            self.blink_started = now
        if self.blink_started is not None:
            elapsed = time.ticks_diff(now, self.blink_started)
            if elapsed >= self.BLINK_DURATION_MS:
                self.blink_started = None
                self.next_blink = time.ticks_add(now, random.randint(2600, 5600))
            else:
                pose = self._close(pose, self._blink_height(elapsed))
        return pose

    def update(self):
        now = time.ticks_ms()
        if self.caption_until is not None and time.ticks_diff(now, self.caption_until) >= 0:
            self.play("neutral")
        if self.move_prompt_until is not None:
            if time.ticks_diff(now, self.move_prompt_until) < 0:
                return
            self.move_prompt_until = None
            self.last_frame = None
        # Fast blinks use elapsed time even if the display misses a frame.
        interval = 10 if self.blink_started is not None else self.FRAME_MS
        if self.last_frame is not None and time.ticks_diff(now, self.last_frame) < interval:
            return
        self.last_frame = now
        elapsed = time.ticks_diff(now, self.started)
        hold_elapsed = max(0, elapsed - self.enter_ms)
        tears = False
        if elapsed < self.enter_ms:
            pose = self._mix(self.from_pose, STYLES[self.expression][0],
                             self._ease(elapsed, self.enter_ms))
        elif self.expression == "neutral":
            pose = self._idle_pose(now)
        elif self.hold_ms is None or hold_elapsed < self.hold_ms:
            pose = self._hold_pose(hold_elapsed)
            tears = self.expression == "crying"
        else:
            if not self.returning:
                # Capture the mathematical end of the hold, even after a long
                # pause (the UART controller skips rendering while driving).
                self.return_pose = self._hold_pose(self.hold_ms)
                self.returning = True
            exit_elapsed = hold_elapsed - self.hold_ms
            pose = self._mix(self.return_pose, NEUTRAL,
                             self._ease(exit_elapsed, self.exit_ms))
            if exit_elapsed >= self.exit_ms:
                self.expression = "neutral"
                self.enter_ms = 0
                self.hold_ms = None
                self._schedule_idle(now)
        color_changed = self._set_color(self._expression_color(elapsed))
        if pose != self.pose or color_changed or tears or self.last_draw_tears or self.last_draw_prompt or self.caption_dirty:
            self.pose = pose
            self._draw(pose, hold_elapsed if tears else None)

    def _draw_question(self, x0, y0, width, height):
        # Round-ended hook and detached dot, drawn with the same framebuffer
        # primitives on LCD and OLED. No font files or extra image buffers.
        points = ((8, 17), (8, 13), (10, 9), (14, 6), (20, 5),
                  (27, 5), (33, 8), (37, 12), (38, 18), (36, 24),
                  (32, 28), (27, 31), (24, 35), (24, 41), (24, 44))
        rx, ry = max(1, width * 5 // 48), max(1, height * 5 // 70)
        for start, end in zip(points, points[1:]):
            steps = max(abs(end[0] - start[0]), abs(end[1] - start[1]), 1)
            for step in range(steps + 1):
                x = start[0] + (end[0] - start[0]) * step // steps
                y = start[1] + (end[1] - start[1]) * step // steps
                self.display.ellipse(x0 + x * width // 48, y0 + y * height // 70,
                                     rx, ry, 1, True)
        self.display.ellipse(x0 + width // 2, y0 + 61 * height // 70,
                             max(1, width * 6 // 48), max(1, height * 6 // 70), 1, True)

    def _draw_eye(self, index, eye):
        dx, dy, width, height, outer, inner, curve, heart, tilt, question = eye
        width = max(4, width * self.CANVAS_WIDTH // 136)
        height = max(3, height * self.CANVAS_HEIGHT // 112)
        center_x = ((36 if index == 0 else 100) + dx) * self.CANVAS_WIDTH // 136
        center_y = (56 + dy) * self.CANVAS_HEIGHT // 112
        x0, y0 = center_x - width // 2, center_y - height // 2
        if question >= 512:
            self._draw_question(x0, y0, width, height)
            return
        radius = min((width - 1) // 2, (height - 1) // 2)
        outer = outer * self.CANVAS_HEIGHT // 112
        inner = inner * self.CANVAS_HEIGHT // 112
        curve = curve * self.CANVAS_HEIGHT // 112
        tilt = tilt * self.CANVAS_HEIGHT // 112
        for column in range(width):
            edge = min(column, width - 1 - column)
            inset = 0
            if edge < radius:
                inset = radius - int(math.sqrt(radius * radius - (radius - edge) ** 2))
            # Rounded capsule, or a curved narrow capsule for closed eyes.
            arch = curve * 4 * column * (width - 1 - column) // ((width - 1) ** 2)
            top, bottom = inset + arch, height - 1 - inset + arch
            fraction = column if index == 0 else width - 1 - column
            lid = outer + (inner - outer) * fraction // (width - 1)
            if outer or inner:
                top = max(top, lid)
            if heart:
                # Two round lobes join a single tapered point. Interpolating
                # column bounds morphs capsules into hearts without a swap.
                u = column / (width - 1)
                lobe = (u - (0.25 if u < 0.5 else 0.75)) / 0.25
                heart_top = int(height * 0.25 * (1 - math.sqrt(max(0, 1 - lobe * lobe))))
                heart_bottom = int((height - 1) * (0.35 + 0.65 * (1 - abs(2 * u - 1))))
                top += (heart_top - top) * heart // 1024
                bottom += (heart_bottom - bottom) * heart // 1024
            if bottom >= top:
                slant = tilt * (2 * column - width + 1) // (2 * (width - 1))
                self.display.fill_rect(x0 + column, y0 + top + slant,
                                       1, bottom - top + 1, 1)

    def _draw(self, pose, tear_elapsed=None):
        self.display.fill(0)
        for index, eye in enumerate(pose):
            self._draw_eye(index, eye)
        if tear_elapsed is not None:
            for x, delay in ((18, 0), (118, 550)):
                age = tear_elapsed - delay
                if age >= 0:
                    phase = age % 1100
                    if phase < 850:
                        y = 82 + 22 * phase // 850
                        self.display.ellipse(x * self.CANVAS_WIDTH // 136,
                                             y * self.CANVAS_HEIGHT // 112,
                                             max(1, 3 * self.CANVAS_WIDTH // 136),
                                             max(2, 5 * self.CANVAS_HEIGHT // 112), 1, True)
        if self.caption:
            y = self.CANVAS_HEIGHT - 12
            self.display.fill_rect(0, y, self.CANVAS_WIDTH, 12, 0)
            self.display.text(self.caption, max(0, (self.CANVAS_WIDTH - len(self.caption) * 8) // 2), y + 2, 1)
        self.caption_dirty = False
        self.display.show()
        self.last_draw_prompt = False
        self.last_draw_tears = tear_elapsed is not None
