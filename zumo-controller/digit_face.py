from zumo_2040_robot import robot
import random
import time


class DigitFace:
    LEFT_EYE_X = 34
    RIGHT_EYE_X = 94
    EYE_Y = 31
    EYE_WIDTH = 23
    EYE_HEIGHT = 27
    BLINK_FRAME_MS = 20
    SAD_FRAME_MS = 50
    SAD_ENTER_FRAMES = 7
    SAD_HOLD_FRAMES = 80
    SAD_EXIT_FRAMES = 3
    HAPPY_FRAME_MS = 20
    HAPPY_COMPRESS_FRAMES = 6
    HAPPY_CURVE_FRAMES = 6
    HAPPY_HOLD_FRAMES = 100
    HAPPY_EXIT_CURVE_FRAMES = 10
    HAPPY_EXPAND_FRAMES = 13
    HAPPY_MOTION_STEP_FRAMES = 5
    HAPPY_LEFT_BOB = (0, -1, -2, -1, 0, 1, 1, 0, -1, 0)
    HAPPY_RIGHT_BOB = (0, -1, -1, -2, -1, 0, 1, 1, 0, 0)
    HAPPY_EXTRA_LIFT = (0, 1, 2, 2, 1, 0, 0, 1, 1, 0)
    MOVE_PROMPT_MS = 2000
    DIZZY_FRAME_MS = 100

    DIZZY_SPIRAL = (
        (-18, -15),
        (18, -15),
        (18, 15),
        (-18, 15),
        (-18, -10),
        (13, -10),
        (13, 10),
        (-13, 10),
        (-13, -5),
        (8, -5),
        (8, 5),
        (-8, 5),
        (-8, 0),
        (3, 0),
    )

    LARGE_FONT = {
        "m": (
            "00000",
            "11010",
            "10101",
            "10101",
            "10101",
            "10101",
            "10101",
        ),
        "o": (
            "00000",
            "01110",
            "10001",
            "10001",
            "10001",
            "10001",
            "01110",
        ),
        "v": (
            "00000",
            "10001",
            "10001",
            "10001",
            "01010",
            "01010",
            "00100",
        ),
        "e": (
            "00000",
            "01110",
            "10001",
            "11111",
            "10000",
            "10001",
            "01110",
        ),
        "?": (
            "01110",
            "10001",
            "00001",
            "00010",
            "00100",
            "00000",
            "00100",
        ),
    }

    BLINK_HEIGHTS = (27, 18, 8, 2, 8, 18, 27)
    LOOK_POSITIONS = (
        (-6, 0),
        (6, 0),
        (0, -3),
        (0, 3),
        (-4, -2),
        (4, -2),
        (0, 0),
    )

    def __init__(self):
        self.display = robot.Display()

        self.look_x = 0
        self.look_y = 0
        self.eye_height = self.EYE_HEIGHT
        self.sad_active = False
        self.sad_started = 0
        self.sad_frame = -1
        self.sad_lid_thickness = 0
        self.tear_y = None
        self.happy_active = False
        self.happy_started = 0
        self.happy_frame = -1
        self.happy_motion_step = -1
        self.move_prompt_active = False
        self.move_prompt_until = 0
        self.dizzy_active = False
        self.dizzy_phase = 0
        self.next_dizzy_frame = 0

        self.blink_frame = -1
        self.look_step = 0

        now = time.ticks_ms()
        self.next_blink = time.ticks_add(now, random.randint(2500, 6000))
        self.next_look = time.ticks_add(now, random.randint(1000, 2500))
        self.next_blink_frame = now
        self.next_look_frame = now

        self._draw()

    def _draw(self):
        self.display.fill(0)
        eye_y = self.EYE_Y + self.look_y

        self.display.ellipse(
            self.LEFT_EYE_X + self.look_x,
            eye_y,
            self.EYE_WIDTH,
            self.eye_height,
            1,
            True,
        )
        self.display.ellipse(
            self.RIGHT_EYE_X + self.look_x,
            eye_y,
            self.EYE_WIDTH,
            self.eye_height,
            1,
            True,
        )

        if self.sad_active and self.sad_lid_thickness:
            # Dark, inward-rising eyelids give the eyes a sad expression.
            for thickness in range(self.sad_lid_thickness):
                self.display.line(11, 16 + thickness, 57, 7 + thickness, 0)
                self.display.line(71, 7 + thickness, 117, 16 + thickness, 0)

        if self.sad_active and self.tear_y is not None:
            self.display.ellipse(53, self.tear_y, 2, 3, 1, True)

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
        first_x, first_y = self._rotate_dizzy_point(
            self.DIZZY_SPIRAL[0][0], self.DIZZY_SPIRAL[0][1], phase
        )
        previous_x = center_x + first_x
        previous_y = center_y + first_y

        for point in self.DIZZY_SPIRAL[1:]:
            point_x, point_y = self._rotate_dizzy_point(
                point[0], point[1], phase
            )
            current_x = center_x + point_x
            current_y = center_y + point_y
            self.display.line(
                previous_x, previous_y, current_x, current_y, 1
            )
            previous_x = current_x
            previous_y = current_y

    def _draw_dizzy(self):
        self.display.fill(0)

        # Large eye outlines frame two counter-rotating spirals.
        self.display.ellipse(
            self.LEFT_EYE_X, self.EYE_Y, self.EYE_WIDTH, self.EYE_HEIGHT, 1
        )
        self.display.ellipse(
            self.RIGHT_EYE_X, self.EYE_Y, self.EYE_WIDTH, self.EYE_HEIGHT, 1
        )
        self._draw_dizzy_spiral(
            self.LEFT_EYE_X, self.EYE_Y, self.dizzy_phase
        )
        self._draw_dizzy_spiral(
            self.RIGHT_EYE_X, self.EYE_Y, (-self.dizzy_phase) % 4
        )
        self.display.show()

    def _draw_happy_curve(
        self,
        progress,
        maximum,
        left_offset_y=0,
        right_offset_y=0,
        extra_lift=0,
    ):
        self.display.fill(0)
        outer_y = self.EYE_Y + 5 * progress // maximum
        shoulder_y = self.EYE_Y - 4 * progress // maximum
        center_y = self.EYE_Y - (9 + extra_lift) * progress // maximum

        for thickness in range(3):
            left_y = thickness + left_offset_y
            right_y = thickness + right_offset_y

            self.display.line(
                14, outer_y + left_y, 22, shoulder_y + left_y, 1
            )
            self.display.line(
                22, shoulder_y + left_y, 34, center_y + left_y, 1
            )
            self.display.line(
                34, center_y + left_y, 46, shoulder_y + left_y, 1
            )
            self.display.line(
                46, shoulder_y + left_y, 54, outer_y + left_y, 1
            )

            self.display.line(
                74, outer_y + right_y, 82, shoulder_y + right_y, 1
            )
            self.display.line(
                82, shoulder_y + right_y, 94, center_y + right_y, 1
            )
            self.display.line(
                94, center_y + right_y, 106, shoulder_y + right_y, 1
            )
            self.display.line(
                106, shoulder_y + right_y, 114, outer_y + right_y, 1
            )

        self.display.show()

    def show_happy(self):
        # Closed, upward-curving eyes create an eyes-only happy expression.
        self.move_prompt_active = False
        self.sad_active = False
        self.happy_active = False
        self.dizzy_active = False
        self.blink_frame = -1
        self.look_step = 0
        self._draw_happy_curve(
            self.HAPPY_CURVE_FRAMES - 1, self.HAPPY_CURVE_FRAMES - 1
        )

    def play_happy(self):
        now = time.ticks_ms()
        self.happy_active = True
        self.happy_started = now
        self.happy_frame = -1
        self.happy_motion_step = -1

        # The happy transition takes priority and starts from centered open eyes.
        self.move_prompt_active = False
        self.sad_active = False
        self.sad_lid_thickness = 0
        self.tear_y = None
        self.dizzy_active = False
        self.blink_frame = -1
        self.look_step = 0
        self.look_x = 0
        self.look_y = 0
        self.eye_height = self.EYE_HEIGHT
        self._update_happy(now)

    def _update_happy(self, now):
        frame = time.ticks_diff(now, self.happy_started) // self.HAPPY_FRAME_MS
        if frame == self.happy_frame:
            return

        self.happy_frame = frame
        curve_start = self.HAPPY_COMPRESS_FRAMES
        hold_start = curve_start + self.HAPPY_CURVE_FRAMES
        exit_curve_start = hold_start + self.HAPPY_HOLD_FRAMES
        expand_start = exit_curve_start + self.HAPPY_EXIT_CURVE_FRAMES
        end_frame = expand_start + self.HAPPY_EXPAND_FRAMES

        if frame < curve_start:
            self.eye_height = self.EYE_HEIGHT - (
                (self.EYE_HEIGHT - 3) * frame
                // (self.HAPPY_COMPRESS_FRAMES - 1)
            )
            self._draw()
        elif frame < hold_start:
            curve_frame = frame - curve_start
            self._draw_happy_curve(
                curve_frame, self.HAPPY_CURVE_FRAMES - 1
            )
        elif frame < exit_curve_start:
            hold_frame = frame - hold_start
            motion_step = hold_frame // self.HAPPY_MOTION_STEP_FRAMES
            if motion_step != self.happy_motion_step:
                self.happy_motion_step = motion_step
                motion_index = motion_step % len(self.HAPPY_LEFT_BOB)
                self._draw_happy_curve(
                    self.HAPPY_CURVE_FRAMES - 1,
                    self.HAPPY_CURVE_FRAMES - 1,
                    self.HAPPY_LEFT_BOB[motion_index],
                    self.HAPPY_RIGHT_BOB[motion_index],
                    self.HAPPY_EXTRA_LIFT[motion_index],
                )
        elif frame < expand_start:
            exit_frame = frame - exit_curve_start
            progress = (
                (self.HAPPY_EXIT_CURVE_FRAMES - 1 - exit_frame)
                * (self.HAPPY_CURVE_FRAMES - 1)
                // (self.HAPPY_EXIT_CURVE_FRAMES - 1)
            )
            self._draw_happy_curve(progress, self.HAPPY_CURVE_FRAMES - 1)
        elif frame < end_frame:
            expand_frame = frame - expand_start
            self.eye_height = 3 + (
                (self.EYE_HEIGHT - 3) * expand_frame
                // (self.HAPPY_EXPAND_FRAMES - 1)
            )
            self._draw()
        else:
            self.happy_active = False
            self.eye_height = self.EYE_HEIGHT
            self.next_blink = time.ticks_add(
                now, random.randint(2500, 6000)
            )
            self.next_look = time.ticks_add(
                now, random.randint(1000, 2500)
            )
            self._draw()

    def start_dizzy(self):
        now = time.ticks_ms()
        self.dizzy_active = True
        self.dizzy_phase = 0
        self.next_dizzy_frame = time.ticks_add(now, self.DIZZY_FRAME_MS)

        # The dizzy face takes priority over other face animations.
        self.move_prompt_active = False
        self.sad_active = False
        self.happy_active = False
        self.sad_lid_thickness = 0
        self.tear_y = None
        self.blink_frame = -1
        self.look_step = 0
        self._draw_dizzy()

    def stop_dizzy(self):
        if not self.dizzy_active:
            return

        now = time.ticks_ms()
        self.dizzy_active = False
        self.look_x = 0
        self.look_y = 0
        self.eye_height = self.EYE_HEIGHT
        self.next_blink = time.ticks_add(now, random.randint(2500, 6000))
        self.next_look = time.ticks_add(now, random.randint(1000, 2500))
        self._draw()

    def _draw_move_prompt(self):
        text = "move?"
        scale = 4
        spacing = 4
        character_width = 5 * scale
        text_width = len(text) * character_width + (len(text) - 1) * spacing
        start_x = (128 - text_width) // 2
        start_y = (64 - 7 * scale) // 2

        self.display.fill(0)

        for character_index, character in enumerate(text):
            character_x = start_x + character_index * (character_width + spacing)
            rows = self.LARGE_FONT[character]

            for row_index, row in enumerate(rows):
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
        now = time.ticks_ms()
        self.move_prompt_active = True
        self.move_prompt_until = time.ticks_add(now, self.MOVE_PROMPT_MS)

        # The prompt takes priority over any face animation in progress.
        self.sad_active = False
        self.happy_active = False
        self.sad_lid_thickness = 0
        self.tear_y = None
        self.blink_frame = -1
        self.look_step = 0

        self._draw_move_prompt()

    def play_sad(self):
        if self.sad_active:
            return

        self.sad_active = True
        self.happy_active = False
        self.sad_started = time.ticks_ms()
        self.sad_frame = -1
        self.sad_lid_thickness = 0
        self.tear_y = None

        # Pause the idle animations and return the eyes to the center.
        self.blink_frame = -1
        self.look_step = 0
        self.look_x = 0
        self.look_y = 0

    def _update_sad(self, now):
        frame = time.ticks_diff(now, self.sad_started) // self.SAD_FRAME_MS
        if frame == self.sad_frame:
            return

        self.sad_frame = frame
        hold_start = self.SAD_ENTER_FRAMES
        exit_start = hold_start + self.SAD_HOLD_FRAMES
        end_frame = exit_start + self.SAD_EXIT_FRAMES

        if frame < hold_start:
            # Compress the eyes first, without showing the sad eyelids yet.
            height_change = 8 * frame // (self.SAD_ENTER_FRAMES - 1)
            self.eye_height = self.EYE_HEIGHT - height_change
            self.sad_lid_thickness = 0
            self.tear_y = None
        elif frame < exit_start:
            # Hold the completed sad expression for four seconds.
            hold_frame = frame - hold_start
            self.eye_height = 19
            self.sad_lid_thickness = min(3, hold_frame + 1)
            self.tear_y = 48 + hold_frame if hold_frame < 13 else None
        elif frame < end_frame:
            # Remove the sad details and return to idle quickly.
            exit_frame = frame - exit_start
            self.eye_height = 19 + 8 * (exit_frame + 1) // self.SAD_EXIT_FRAMES
            self.sad_lid_thickness = max(0, 2 - exit_frame)
            self.tear_y = None
        else:
            self.sad_active = False
            self.eye_height = self.EYE_HEIGHT
            self.sad_lid_thickness = 0
            self.tear_y = None
            self.next_blink = time.ticks_add(now, random.randint(2500, 6000))
            self.next_look = time.ticks_add(now, random.randint(1000, 2500))

        self._draw()

    def _start_look(self, now):
        target = self.LOOK_POSITIONS[
            random.randint(0, len(self.LOOK_POSITIONS) - 1)
        ]
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
                self.next_dizzy_frame = time.ticks_add(
                    now, self.DIZZY_FRAME_MS
                )
                self._draw_dizzy()
            return

        if self.move_prompt_active:
            if time.ticks_diff(now, self.move_prompt_until) >= 0:
                self.move_prompt_active = False
                self.look_x = 0
                self.look_y = 0
                self.eye_height = self.EYE_HEIGHT
                self.next_blink = time.ticks_add(
                    now, random.randint(2500, 6000)
                )
                self.next_look = time.ticks_add(
                    now, random.randint(1000, 2500)
                )
                self._draw()
            return

        if self.sad_active:
            self._update_sad(now)
            return

        if self.happy_active:
            self._update_happy(now)
            return

        redraw = False

        if self.blink_frame >= 0:
            if time.ticks_diff(now, self.next_blink_frame) >= 0:
                self.eye_height = self.BLINK_HEIGHTS[self.blink_frame]
                self.blink_frame += 1
                self.next_blink_frame = time.ticks_add(now, self.BLINK_FRAME_MS)
                redraw = True

                if self.blink_frame == len(self.BLINK_HEIGHTS):
                    self.blink_frame = -1
                    self.eye_height = self.EYE_HEIGHT
                    self.next_blink = time.ticks_add(
                        now, random.randint(2500, 6000)
                    )
        elif time.ticks_diff(now, self.next_blink) >= 0:
            self.blink_frame = 0
            self.next_blink_frame = now

        if self.look_step > 0:
            if time.ticks_diff(now, self.next_look_frame) >= 0:
                steps = 6
                self.look_x = self.look_start_x + (
                    (self.look_target_x - self.look_start_x) * self.look_step // steps
                )
                self.look_y = self.look_start_y + (
                    (self.look_target_y - self.look_start_y) * self.look_step // steps
                )
                redraw = True

                if self.look_step == steps:
                    self.look_step = 0
                    self.next_look = time.ticks_add(
                        now, random.randint(1200, 3500)
                    )
                else:
                    self.look_step += 1
                    self.next_look_frame = time.ticks_add(now, 45)
        elif time.ticks_diff(now, self.next_look) >= 0:
            self._start_look(now)

        if redraw:
            self._draw()
