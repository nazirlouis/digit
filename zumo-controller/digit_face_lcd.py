"""High-resolution animated face for Digit's 1.69-inch color LCD."""

from digit_display import Display
import random
import time


class DigitFace:
    CANVAS_WIDTH = 136
    CANVAS_HEIGHT = 112
    ORANGE = 0xFCE0
    BACKGROUND = 0x0000

    LEFT_EYE_X = 12
    RIGHT_EYE_X = 76
    EYE_CENTER_Y = 56
    EYE_WIDTH = 48
    EYE_HEIGHT = 70
    # Match Pixel Plus's LV_RADIUS_CIRCLE treatment.  Since the eyes are
    # taller than they are wide, half the width makes a tall capsule rather
    # than a circle.
    CORNER_RADIUS = EYE_WIDTH // 2

    FRAME_MS = 16
    BLINK_DURATION_MS = 110
    BLINK_REDRAW_MS = 10
    BLINK_MIN_HEIGHT = 6
    LOOK_POSITIONS = (
        (-7, 0), (7, 0), (0, -5), (0, 5),
        (-5, -3), (5, -3), (-4, 3), (4, 3), (0, 0),
    )

    HAPPY_COMPRESS_MS = 180
    HAPPY_CURVE_MS = 180
    HAPPY_HOLD_MS = 2000
    HAPPY_EXIT_CURVE_MS = 220
    HAPPY_EXPAND_MS = 260
    HAPPY_BOB_MS = 48
    HAPPY_BOB = (0, -1, -2, -2, -1, 0, 1, 2, 2, 1, 0, -1)

    SAD_ENTER_MS = 320
    SAD_HOLD_MS = 4000
    SAD_EXIT_MS = 280
    DIZZY_FRAME_MS = 80
    MOVE_PROMPT_MS = 2000

    DIZZY_SPIRAL = (
        (-19, -20), (19, -20), (19, 20), (-19, 20),
        (-19, -14), (14, -14), (14, 14), (-14, 14),
        (-14, -8), (8, -8), (8, 8), (-8, 8),
        (-8, -3), (3, -3), (3, 3),
    )

    LARGE_FONT = {
        "m": ("00000", "11010", "10101", "10101", "10101", "10101", "10101"),
        "o": ("00000", "01110", "10001", "10001", "10001", "10001", "01110"),
        "v": ("00000", "10001", "10001", "10001", "01010", "01010", "00100"),
        "e": ("00000", "01110", "10001", "11111", "10000", "10001", "01110"),
        "?": ("01110", "10001", "00001", "00010", "00100", "00000", "00100"),
    }

    def __init__(self, display=None):
        self.display = display or Display(
            width=self.CANVAS_WIDTH,
            height=self.CANVAS_HEIGHT,
            foreground=self.ORANGE,
            background=self.BACKGROUND,
        )
        self.display.set_colors(self.ORANGE, self.BACKGROUND)

        self.look_x = 0
        self.look_y = 0
        self.look_start_x = 0
        self.look_start_y = 0
        self.look_target_x = 0
        self.look_target_y = 0
        self.look_step = 0
        self.eye_height = self.EYE_HEIGHT

        self.blink_started = None
        self.happy_active = False
        self.sad_active = False
        self.dizzy_active = False
        self.move_prompt_active = False
        self.animation_frame = -1
        self.dizzy_phase = 0

        now = time.ticks_ms()
        self.next_blink = time.ticks_add(now, random.randint(2600, 5600))
        self.next_blink_redraw = now
        self.next_look = time.ticks_add(now, random.randint(1000, 2400))
        self.next_look_frame = now
        self.next_dizzy_frame = now
        self._draw_idle()

    @staticmethod
    def _ease(progress, duration):
        if progress <= 0:
            return 0
        if progress >= duration:
            return 1024
        value = progress * 1024 // duration
        return value * value * (3072 - 2 * value) // (1024 * 1024)

    def _fill_round_rect(self, x, y, width, height, radius, color=1):
        if width <= 0 or height <= 0:
            return
        # Pixel centers come in pairs on even-sized shapes.  Clamping to the
        # half-size minus one keeps the ellipses inside the requested bounds
        # while still producing the same fully rounded capsule silhouette.
        radius = min(radius, (width - 1) // 2, (height - 1) // 2)
        if radius <= 1:
            self.display.fill_rect(x, y, width, height, color)
            return
        self.display.fill_rect(x + radius, y, width - 2 * radius, height, color)
        self.display.fill_rect(x, y + radius, width, height - 2 * radius, color)
        left = x + radius
        right = x + width - radius - 1
        top = y + radius
        bottom = y + height - radius - 1
        self.display.ellipse(left, top, radius, radius, color, True)
        self.display.ellipse(right, top, radius, radius, color, True)
        self.display.ellipse(left, bottom, radius, radius, color, True)
        self.display.ellipse(right, bottom, radius, radius, color, True)

    def _draw_eye(self, x, offset_y=0):
        height = max(3, self.eye_height)
        y = self.EYE_CENTER_Y + self.look_y + offset_y - height // 2
        radius = min(self.CORNER_RADIUS, height // 2)
        self._fill_round_rect(
            x + self.look_x, y, self.EYE_WIDTH, height, radius, 1
        )

    def _blink_height(self, elapsed):
        """Return blink height from wall-clock time, not rendered frames."""
        duration = self.BLINK_DURATION_MS
        if elapsed <= 0 or elapsed >= duration:
            return self.EYE_HEIGHT
        half = max(1, duration // 2)
        if elapsed <= half:
            closure = elapsed * 1024 // half
        else:
            closure = (duration - elapsed) * 1024 // max(1, duration - half)
        return self.EYE_HEIGHT - (
            (self.EYE_HEIGHT - self.BLINK_MIN_HEIGHT) * closure // 1024
        )

    def _draw_idle(self, left_offset_y=0, right_offset_y=0,
                   sad_amount=0, tear_y=None):
        self.display.fill(0)
        self._draw_eye(self.LEFT_EYE_X, left_offset_y)
        self._draw_eye(self.RIGHT_EYE_X, right_offset_y)

        if sad_amount:
            thickness = 1 + 7 * sad_amount // 1024
            left_top = self.EYE_CENTER_Y - self.eye_height // 2
            for offset in range(thickness):
                self.display.line(
                    self.LEFT_EYE_X,
                    left_top + 18 + offset,
                    self.LEFT_EYE_X + self.EYE_WIDTH - 1,
                    left_top + 5 + offset,
                    0,
                )
                self.display.line(
                    self.RIGHT_EYE_X,
                    left_top + 5 + offset,
                    self.RIGHT_EYE_X + self.EYE_WIDTH - 1,
                    left_top + 18 + offset,
                    0,
                )
        if tear_y is not None:
            self.display.ellipse(
                self.LEFT_EYE_X + self.EYE_WIDTH - 3,
                tear_y,
                3,
                5,
                1,
                True,
            )
        self.display.show()

    def _draw_happy(self, amount=1024, left_offset=0, right_offset=0):
        self.display.fill(0)
        lift = 20 * amount // 1024
        half = self.EYE_WIDTH // 2
        for eye_x, offset_y in (
            (self.LEFT_EYE_X, left_offset),
            (self.RIGHT_EYE_X, right_offset),
        ):
            previous_x = eye_x
            previous_dx = -half
            previous_y = 67 + offset_y - lift * (
                half * half - previous_dx * previous_dx
            ) // (half * half)
            for local_x in range(4, self.EYE_WIDTH + 1, 4):
                dx = local_x - half
                current_x = eye_x + min(local_x, self.EYE_WIDTH - 1)
                current_y = 67 + offset_y - lift * (
                    half * half - dx * dx
                ) // (half * half)
                for thickness in range(-2, 3):
                    self.display.line(
                        previous_x,
                        previous_y + thickness,
                        current_x,
                        current_y + thickness,
                        1,
                    )
                previous_x = current_x
                previous_y = current_y
        self.display.show()

    def _rotate_dizzy_point(self, x, y, phase):
        if phase == 1:
            return -y, x
        if phase == 2:
            return -x, -y
        if phase == 3:
            return y, -x
        return x, y

    def _draw_dizzy_spiral(self, center_x, center_y, phase):
        first = self._rotate_dizzy_point(
            self.DIZZY_SPIRAL[0][0], self.DIZZY_SPIRAL[0][1], phase
        )
        previous_x = center_x + first[0]
        previous_y = center_y + first[1]
        for x, y in self.DIZZY_SPIRAL[1:]:
            x, y = self._rotate_dizzy_point(x, y, phase)
            current_x = center_x + x
            current_y = center_y + y
            self.display.line(previous_x, previous_y, current_x, current_y, 1)
            previous_x, previous_y = current_x, current_y

    def _draw_dizzy(self):
        self.display.fill(0)
        for center_x, phase in (
            (self.LEFT_EYE_X + self.EYE_WIDTH // 2, self.dizzy_phase),
            (self.RIGHT_EYE_X + self.EYE_WIDTH // 2, (-self.dizzy_phase) % 4),
        ):
            self._fill_round_rect(
                center_x - self.EYE_WIDTH // 2,
                self.EYE_CENTER_Y - self.EYE_HEIGHT // 2,
                self.EYE_WIDTH,
                self.EYE_HEIGHT,
                self.CORNER_RADIUS,
                1,
            )
            self._fill_round_rect(
                center_x - self.EYE_WIDTH // 2 + 5,
                self.EYE_CENTER_Y - self.EYE_HEIGHT // 2 + 5,
                self.EYE_WIDTH - 10,
                self.EYE_HEIGHT - 10,
                self.CORNER_RADIUS - 4,
                0,
            )
            self._draw_dizzy_spiral(center_x, self.EYE_CENTER_Y, phase)
        self.display.show()

    def _cancel_expressions(self):
        self.happy_active = False
        self.sad_active = False
        self.dizzy_active = False
        self.move_prompt_active = False
        self.blink_started = None
        self.look_step = 0

    def _resume_idle(self, now):
        self.blink_started = None
        self.eye_height = self.EYE_HEIGHT
        self.look_x = 0
        self.look_y = 0
        self.next_blink = time.ticks_add(now, random.randint(2600, 5600))
        self.next_look = time.ticks_add(now, random.randint(1000, 2400))
        self._draw_idle()

    def show_happy(self):
        self._cancel_expressions()
        self._draw_happy()

    def play_happy(self):
        self._cancel_expressions()
        self.happy_active = True
        self.happy_started = time.ticks_ms()
        self.animation_frame = -1

    def _update_happy(self, now):
        elapsed = time.ticks_diff(now, self.happy_started)
        frame = elapsed // self.FRAME_MS
        if frame == self.animation_frame:
            return
        self.animation_frame = frame

        curve_start = self.HAPPY_COMPRESS_MS
        hold_start = curve_start + self.HAPPY_CURVE_MS
        exit_start = hold_start + self.HAPPY_HOLD_MS
        expand_start = exit_start + self.HAPPY_EXIT_CURVE_MS
        end = expand_start + self.HAPPY_EXPAND_MS

        if elapsed < curve_start:
            eased = self._ease(elapsed, self.HAPPY_COMPRESS_MS)
            self.eye_height = self.EYE_HEIGHT - (
                (self.EYE_HEIGHT - 5) * eased // 1024
            )
            self._draw_idle()
        elif elapsed < hold_start:
            eased = self._ease(elapsed - curve_start, self.HAPPY_CURVE_MS)
            self._draw_happy(eased)
        elif elapsed < exit_start:
            bob_index = (elapsed - hold_start) // self.HAPPY_BOB_MS
            left = self.HAPPY_BOB[bob_index % len(self.HAPPY_BOB)]
            right = self.HAPPY_BOB[(bob_index + 4) % len(self.HAPPY_BOB)]
            self._draw_happy(1024, left, right)
        elif elapsed < expand_start:
            eased = self._ease(elapsed - exit_start, self.HAPPY_EXIT_CURVE_MS)
            self._draw_happy(1024 - eased)
        elif elapsed < end:
            eased = self._ease(elapsed - expand_start, self.HAPPY_EXPAND_MS)
            self.eye_height = 5 + (self.EYE_HEIGHT - 5) * eased // 1024
            self._draw_idle()
        else:
            self.happy_active = False
            self._resume_idle(now)

    def play_sad(self):
        if self.sad_active:
            return
        self._cancel_expressions()
        self.sad_active = True
        self.sad_started = time.ticks_ms()
        self.animation_frame = -1
        self.look_x = 0
        self.look_y = 0

    def _update_sad(self, now):
        elapsed = time.ticks_diff(now, self.sad_started)
        frame = elapsed // self.FRAME_MS
        if frame == self.animation_frame:
            return
        self.animation_frame = frame
        hold_start = self.SAD_ENTER_MS
        exit_start = hold_start + self.SAD_HOLD_MS
        end = exit_start + self.SAD_EXIT_MS

        if elapsed < hold_start:
            amount = self._ease(elapsed, self.SAD_ENTER_MS)
            self.eye_height = self.EYE_HEIGHT - 14 * amount // 1024
            self._draw_idle(sad_amount=amount)
        elif elapsed < exit_start:
            hold_elapsed = elapsed - hold_start
            tear_y = None
            if hold_elapsed < 850:
                tear_y = 72 + 25 * hold_elapsed // 850
            self.eye_height = self.EYE_HEIGHT - 14
            self._draw_idle(sad_amount=1024, tear_y=tear_y)
        elif elapsed < end:
            amount = 1024 - self._ease(elapsed - exit_start, self.SAD_EXIT_MS)
            self.eye_height = self.EYE_HEIGHT - 14 * amount // 1024
            self._draw_idle(sad_amount=amount)
        else:
            self.sad_active = False
            self._resume_idle(now)

    def start_dizzy(self):
        self._cancel_expressions()
        self.dizzy_active = True
        self.dizzy_phase = 0
        now = time.ticks_ms()
        self.next_dizzy_frame = time.ticks_add(now, self.DIZZY_FRAME_MS)
        self._draw_dizzy()

    def stop_dizzy(self):
        if self.dizzy_active:
            self.dizzy_active = False
            self._resume_idle(time.ticks_ms())

    def _draw_move_prompt(self):
        text = "move?"
        scale = 4
        spacing = 4
        character_width = 5 * scale
        text_width = len(text) * character_width + (len(text) - 1) * spacing
        start_x = (self.CANVAS_WIDTH - text_width) // 2
        start_y = (self.CANVAS_HEIGHT - 7 * scale) // 2
        self.display.fill(0)
        for character_index, character in enumerate(text):
            character_x = start_x + character_index * (character_width + spacing)
            for row_index, row in enumerate(self.LARGE_FONT[character]):
                for column_index, pixel in enumerate(row):
                    if pixel == "1":
                        self.display.fill_rect(
                            character_x + column_index * scale,
                            start_y + row_index * scale,
                            scale,
                            scale,
                            1,
                        )
        self.display.show()

    def show_move_prompt(self):
        self._cancel_expressions()
        self.move_prompt_active = True
        self.move_prompt_until = time.ticks_add(
            time.ticks_ms(), self.MOVE_PROMPT_MS
        )
        self._draw_move_prompt()

    def _start_look(self, now):
        target = self.LOOK_POSITIONS[random.randint(0, len(self.LOOK_POSITIONS) - 1)]
        self.look_start_x = self.look_x
        self.look_start_y = self.look_y
        self.look_target_x = target[0]
        self.look_target_y = target[1]
        self.look_step = 1
        self.next_look_frame = now

    def update(self):
        now = time.ticks_ms()

        if self.dizzy_active:
            if time.ticks_diff(now, self.next_dizzy_frame) >= 0:
                self.dizzy_phase = (self.dizzy_phase + 1) % 4
                self.next_dizzy_frame = time.ticks_add(now, self.DIZZY_FRAME_MS)
                self._draw_dizzy()
            return
        if self.move_prompt_active:
            if time.ticks_diff(now, self.move_prompt_until) >= 0:
                self.move_prompt_active = False
                self._resume_idle(now)
            return
        if self.sad_active:
            self._update_sad(now)
            return
        if self.happy_active:
            self._update_happy(now)
            return

        redraw = False
        if self.blink_started is not None:
            elapsed = time.ticks_diff(now, self.blink_started)
            if elapsed >= self.BLINK_DURATION_MS:
                self.blink_started = None
                if self.eye_height != self.EYE_HEIGHT:
                    self.eye_height = self.EYE_HEIGHT
                    redraw = True
                self.next_blink = time.ticks_add(
                    now, random.randint(2600, 5600)
                )
            elif time.ticks_diff(now, self.next_blink_redraw) >= 0:
                height = self._blink_height(elapsed)
                if height != self.eye_height:
                    self.eye_height = height
                    redraw = True
                self.next_blink_redraw = time.ticks_add(
                    now, self.BLINK_REDRAW_MS
                )
        elif time.ticks_diff(now, self.next_blink) >= 0:
            self.blink_started = now
            self.next_blink_redraw = now

        if self.look_step > 0:
            if time.ticks_diff(now, self.next_look_frame) >= 0:
                steps = 12
                eased = self._ease(self.look_step, steps)
                self.look_x = self.look_start_x + (
                    (self.look_target_x - self.look_start_x) * eased // 1024
                )
                self.look_y = self.look_start_y + (
                    (self.look_target_y - self.look_start_y) * eased // 1024
                )
                redraw = True
                if self.look_step == steps:
                    self.look_step = 0
                    self.next_look = time.ticks_add(
                        now, random.randint(1200, 3200)
                    )
                else:
                    self.look_step += 1
                    self.next_look_frame = time.ticks_add(now, self.FRAME_MS)
        elif time.ticks_diff(now, self.next_look) >= 0:
            self._start_look(now)

        if redraw:
            self._draw_idle()
