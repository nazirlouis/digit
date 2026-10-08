"""Render and timeline regressions; no device or motor access."""

import pathlib
import re
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from digit_face_engine import (FaceEngine, EXPRESSIONS, STYLES, NEUTRAL,
                               COLORS, SURPRISE_FLASH_COLOR, SURPRISE_FLASH_MS)
from test_digit_display import FrameBuffer


class Display(FrameBuffer):
    def __init__(self, width=136, height=112):
        super().__init__(bytearray(width * height // 8), width, height, 0)
        self.shows = 0
        self.clipped = False

    def fill_rect(self, x, y, width, height, value):
        if x < 0 or y < 0 or x + width > self.w or y + height > self.h:
            self.clipped = True
        super().fill_rect(x, y, width, height, value)

    def show(self):
        self.shows += 1

    def text(self, text, x, y, value):
        self.fill_rect(x, y, len(text) * 8, 8, value)


class CooperativeDisplay(Display):
    def __init__(self):
        super().__init__()
        self.frame_pending = False
        self.frame_id = 0
        self.completed_frame_id = 0
        self.primitives = 0
        self.queued = None
        self.cancels = 0

    def enable_cooperative(self):
        pass

    def cancel_frame(self):
        self.frame_pending = False
        self.cancels += 1

    def fill(self, value):
        self.primitives += 1
        super().fill(value)

    def fill_rect(self, *args):
        self.primitives += 1
        super().fill_rect(*args)

    def ellipse(self, *args):
        self.primitives += 1
        super().ellipse(*args)

    def show(self):
        super().show()
        self.frame_id += 1
        self.frame_pending = True
        self.queued = bytes(self.buf)
        return self.frame_id

    def flush(self, **kwargs):
        self.completed_frame_id = self.frame_id
        self.frame_pending = False


class FaceTests(unittest.TestCase):
    def test_speaker_timelines_match_face_timelines(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "xiao-sense/src/emotion_voice.h").read_text(encoding="utf-8")
        durations = [int(value) for value in re.search(r'expression\[\] = \{([^}]+)\}', source)[1].split(",")]
        self.assertEqual(len(durations), len(EXPRESSIONS))
        for name, duration in zip(EXPRESSIONS, durations):
            _, enter, hold, exit_ms = STYLES[name]
            expected = enter+3200 if name == "sleeping" else enter if hold is None else enter+hold+exit_ms
            self.assertEqual(duration, expected, name)
        from wake_reaction import DURATION_MS
        wake = int(re.search(r'VoiceTiming::Wake \? (\d+)', source)[1])
        self.assertEqual(wake, DURATION_MS)

    def test_action_scene_finishes_its_reaction_but_keeps_the_caption(self):
        for name in EXPRESSIONS:
            if name in ("neutral", "sleeping"):
                continue
            self.face.show_scene(name, "FOUND")
            _, enter, hold, exit_ms = STYLES[name]
            self.advance(enter + hold + exit_ms + 30)
            self.assertEqual(self.face.expression, "neutral", name)
            self.assertEqual(self.face.caption, "FOUND", name)
        self.face.show_scene("sleeping", "NONE")
        self.advance(20000)
        self.assertEqual(self.face.expression, "sleeping")

    def test_scene_labels_fit_stay_visible_and_clear_on_stop_or_expiry(self):
        for label in ("BATTERY", "MARKER", "SCREWDRIVER", "NOT_SURE"):
            self.face.show_scene("happy", label)
            self.assertEqual(self.face.caption, label.replace("_", " "))
            self.assertFalse(self.display.clipped)
            self.advance(1000)
            self.assertTrue(self.face.caption)
            self.advance(7100)
            self.assertEqual(self.face.caption, "")
        self.face.show_scene("excited", "FOUND")
        self.face.play("neutral")
        self.advance(200)
        self.assertEqual(self.face.caption, "")

    def setUp(self):
        self.now = 0
        for item in (
            patch.object(time, "ticks_ms", lambda: self.now, create=True),
            patch.object(time, "ticks_us", lambda: self.now * 1000, create=True),
            patch.object(time, "ticks_add", lambda a, b: (a + b) % (1 << 30), create=True),
            patch.object(time, "ticks_diff", lambda a, b: ((a - b + (1 << 29)) % (1 << 30)) - (1 << 29), create=True),
        ):
            item.start()
            self.addCleanup(item.stop)
        self.display = Display()
        self.face = FaceEngine(self.display)

    def advance(self, delta):
        self.now = (self.now + delta) % (1 << 30)
        self.face.update()

    def test_every_expression_renders_distinct_visible_eyes_on_both_sizes(self):
        for width, height in ((136, 112), (128, 64)):
            engine = type("SizedFace", (FaceEngine,), {"CANVAS_WIDTH": width, "CANVAS_HEIGHT": height})
            display = Display(width, height)
            face = engine(display)
            frames = {}
            for name in EXPRESSIONS:
                face.play(name)
                self.now += STYLES[name][1] + 250
                face.update()
                self.assertTrue(any(display.buf), name)
                self.assertFalse(display.clipped, name)
                frames[name] = bytes(display.buf)
            self.assertEqual(len(set(frames.values())), len(EXPRESSIONS))

    def test_sad_has_two_connected_eyes_without_detached_caps_or_tears(self):
        self.face.play("sad")
        for delta in (60, 120, 240, 1000):
            self.advance(delta)
            pixels = {(x, y) for y in range(112) for x in range(136)
                      if self.display.pixel(x, y)}
            components = 0
            while pixels:
                components += 1
                queue = [pixels.pop()]
                while queue:
                    x, y = queue.pop()
                    for point in ((x-1, y), (x+1, y), (x, y-1), (x, y+1)):
                        if point in pixels:
                            pixels.remove(point)
                            queue.append(point)
            self.assertEqual(components, 2)

    def test_finite_expressions_finish_after_missed_frames(self):
        for name in EXPRESSIONS:
            if name in ("neutral", "sleeping"):
                continue
            self.face.play(name)
            self.advance(20000)
            self.assertEqual(self.face.expression, "neutral", name)
            self.assertEqual(self.face.pose, NEUTRAL, name)

    def test_sleep_persists_and_can_be_interrupted_without_an_instant_swap(self):
        self.face.play("sleeping")
        self.advance(120000)
        self.assertEqual(self.face.expression, "sleeping")
        before = bytes(self.display.buf)
        self.face.play("surprised")
        self.face.update()
        self.assertEqual(bytes(self.display.buf), before)
        self.advance(130)
        self.assertNotEqual(bytes(self.display.buf), before)
        self.face.play("neutral")
        self.advance(240)
        self.assertEqual(self.face.pose, NEUTRAL)

    def test_sleepy_reopens_and_excited_changes_during_hold(self):
        for name, steps in (("sleepy", (1900, 1300)), ("excited", (600, 600))):
            self.face.play(name)
            self.advance(STYLES[name][1])
            first = bytes(self.display.buf)
            self.advance(steps[0])
            self.assertNotEqual(bytes(self.display.buf), first)
            self.advance(steps[1])
            self.assertEqual(bytes(self.display.buf), first)

    def test_time_wrap_and_blink_completion(self):
        self.now = (1 << 30) - 50
        self.face.play("happy")
        self.advance(4000)
        self.assertEqual(self.face.expression, "neutral")
        self.face.next_blink = self.now
        self.advance(30)
        self.advance(55)
        self.assertEqual(self.face.pose[0][3], self.face.BLINK_MIN_HEIGHT)
        self.advance(55)
        self.assertEqual(self.face.pose[0][3], self.face.EYE_HEIGHT)

    def test_prompt_clears_and_unknown_expression_preserves_face(self):
        self.face.show_move_prompt()
        self.advance(2100)
        self.assertFalse(self.face.last_draw_prompt)
        before = bytes(self.display.buf)
        with self.assertRaises(ValueError):
            self.face.play("dizzy")
        self.assertEqual(bytes(self.display.buf), before)

    def test_switch_from_crying_to_sad_removes_tears_even_when_eyes_match(self):
        self.face.play("crying")
        self.advance(1000)
        crying = bytes(self.display.buf)
        self.face.play("sad")
        self.face.update()
        self.assertNotEqual(bytes(self.display.buf), crying)
        self.assertFalse(self.face.last_draw_tears)
        self.assertEqual(self.face.pose, STYLES["sad"][0])

    def test_color_change_redraws_static_pixels_and_only_rebuilds_on_change(self):
        changes = []
        self.display.set_colors = lambda fg, bg: changes.append((fg, bg))
        self.face = FaceEngine(self.display)
        original = bytes(self.display.buf)
        shown = self.display.shows
        self.face.play("mad")
        self.face.update()
        self.assertEqual(bytes(self.display.buf), original)
        self.assertEqual(self.display.shows, shown + 1)
        self.assertEqual(changes, [(COLORS["neutral"], 0), (0xF800, 0)])
        for _ in range(12):
            self.advance(60)
        self.assertEqual(len(changes), 2)
        self.advance(4000)
        self.assertEqual(changes[-1], (COLORS["neutral"], 0))

    def test_surprised_flashes_once_then_settles_and_resets_to_orange(self):
        self.face.play("surprised")
        self.face.update()
        self.assertEqual(self.face.color, SURPRISE_FLASH_COLOR)
        self.advance(130)
        flashed_pose = self.face.pose
        self.advance(SURPRISE_FLASH_MS - 130)
        self.assertEqual(self.face.pose, flashed_pose)
        self.assertEqual(self.face.color, COLORS["surprised"])
        for _ in range(10):
            self.advance(60)
            self.assertEqual(self.face.color, COLORS["surprised"])
        self.advance(4000)
        self.assertEqual(self.face.color, COLORS["neutral"])

    def test_interrupted_flash_and_sleep_do_not_leave_stale_colors(self):
        self.face.play("surprised")
        self.face.update()
        self.face.play("sleeping")
        self.face.update()
        self.advance(10000)
        self.assertEqual(self.face.color, COLORS["sleeping"])
        self.face.play("neutral")
        self.face.update()
        self.assertEqual(self.face.color, COLORS["neutral"])
        self.face.play("surprised")
        self.advance(500)  # A missed flash is not replayed late.
        self.assertEqual(self.face.color, COLORS["surprised"])

    def test_immediate_happy_and_move_prompt_set_their_own_colors(self):
        self.face.play("mad")
        self.face.update()
        self.face.show_happy()
        self.assertEqual(self.face.color, COLORS["happy"])
        self.face.show_move_prompt()
        self.assertEqual(self.face.color, COLORS["neutral"])

    def cooperative_face(self):
        display = CooperativeDisplay()
        face = FaceEngine(display)
        self.assertTrue(face.enable_cooperative())
        display.cancel_frame()  # Constructor's synchronous intro is already visible.
        return face, display

    def drain_face(self, face):
        for _ in range(200):
            if not face.frame_pending:
                return
            self.now += 5
            face.update(max_units=8, max_us=1500)
        self.fail("cooperative face never completed")

    def test_scene_acknowledgement_does_not_mean_visible_and_drawing_is_bounded(self):
        face, display = self.cooperative_face()
        shown, primitives = display.shows, display.primitives
        face.show_scene("confused", "FOUND")
        self.assertEqual(display.shows, shown)
        self.assertEqual(display.primitives, primitives)
        self.assertEqual(face.scene_sequence, 1)
        self.assertEqual(face.visible_scene_sequence, 0)
        while face._render_steps is not None:
            before = display.primitives
            face.update(max_units=4, max_us=1500)
            self.assertLessEqual(display.primitives - before, 4)
            self.assertEqual(face.visible_scene_sequence, 0)
        self.assertTrue(display.frame_pending)
        self.now += 500
        face.update()
        self.assertEqual(face.visible_scene_sequence, 1)
        self.assertEqual(face.started, self.now)

    def test_cooperative_scenes_preserve_all_approved_expression_pixels(self):
        face, display = self.cooperative_face()
        for name in EXPRESSIONS:
            self.face.show_scene(name, "FOUND")
            face.show_scene(name, "FOUND")
            self.drain_face(face)
            self.assertEqual(display.queued, bytes(self.display.buf), name)
            self.assertEqual(face.visible_scene_sequence, face.scene_sequence)

    def test_interruption_discards_unfinished_drawing_and_pending_scene_visibility(self):
        face, display = self.cooperative_face()
        face.show_scene("confused", "OLD")
        face.update(max_units=4)
        face.show_scene("happy", "NEW")
        self.drain_face(face)
        self.face.show_scene("happy", "NEW")
        self.assertEqual(display.queued, bytes(self.display.buf))
        self.assertEqual(face.visible_scene_sequence, 2)
        face.show_scene("sad", "OLD")
        while face._render_steps is not None:
            face.update()
        face.play("neutral")
        self.assertFalse(display.frame_pending)
        face.update()
        self.drain_face(face)
        self.assertEqual(face.visible_scene_sequence, 2)
        self.assertEqual(face.caption, "")

    def test_cooperative_prompt_is_scheduled_and_legacy_display_is_not_opted_in(self):
        self.assertFalse(self.face.enable_cooperative())
        face, display = self.cooperative_face()
        before = display.primitives
        face.show_move_prompt()
        self.assertEqual(display.primitives, before)
        self.drain_face(face)
        self.face.show_move_prompt()
        self.assertEqual(display.queued, bytes(self.display.buf))


if __name__ == "__main__":
    unittest.main()
