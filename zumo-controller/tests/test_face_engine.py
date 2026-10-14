"""Render and timeline regressions; no device or motor access."""

import pathlib
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from digit_face_engine import (FaceEngine, EXPRESSIONS, STYLES, NEUTRAL,
                               COLORS)
from reaction_catalog import TIMINGS
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
        self.assertEqual(face.enter_ms, TIMINGS["confused"][0])
        self.assertEqual(face.pose, NEUTRAL)
        self.assertEqual(face.caption_until, self.now + 8000)

    def test_slow_first_frame_does_not_consume_reaction_or_caption_lifetime(self):
        face, display = self.cooperative_face()
        face.show_scene("happy", "FOUND")
        self.now += 1500
        self.drain_face(face)
        started = face.started
        self.assertEqual(face.active_scene_sequence, 1)
        self.assertEqual(face.visible_scene_sequence, 1)
        self.assertEqual(face.caption_until, started + 8000)
        self.now = started + sum(TIMINGS["happy"]) - 1
        face.update()
        self.assertEqual(face.expression, "happy")
        self.drain_face(face)
        self.now += 30
        face.update()
        self.assertEqual(face.expression, "neutral")
        self.assertEqual(face.caption, "FOUND")

    def test_visible_scene_hold_preserves_applied_frame_and_release_starts_exit(self):
        face, display = self.cooperative_face()
        face.show_scene("happy", "FOUND")
        self.assertFalse(face.hold_scene(face.scene_sequence))
        self.drain_face(face)
        sequence, started = face.scene_sequence, face.started
        shown, pixels, frame = display.shows, display.queued, display.frame_id
        self.assertTrue(face.hold_scene(sequence))
        self.assertTrue(face.hold_scene(sequence))
        self.assertEqual((face.scene_sequence, face.visible_scene_sequence, face.started),
                         (sequence, sequence, started))
        self.assertEqual((display.shows, display.queued, display.frame_id), (shown, pixels, frame))
        self.now = started + 9000
        face.update(); self.drain_face(face)
        self.assertEqual(face.expression, "happy")
        self.assertEqual(face.caption, "FOUND")
        before = face.pose
        self.assertTrue(face.release_scene(sequence))
        self.assertEqual(face.pose, before)
        self.assertEqual(face.return_pose, before)
        self.assertFalse(face.release_scene(sequence))
        self.now += TIMINGS["happy"][2] + 30
        face.update(); self.drain_face(face)
        self.assertEqual(face.expression, "neutral")
        self.assertEqual(face.caption, "")

    def test_old_hold_release_cannot_overwrite_a_replacement_scene_or_face(self):
        face, _ = self.cooperative_face()
        for replacement in ("scene", "face"):
            face.show_scene("happy", "FOUND"); self.drain_face(face)
            old = face.scene_sequence
            self.assertTrue(face.hold_scene(old))
            if replacement == "scene":
                face.show_scene("sad", "NEW"); self.drain_face(face)
            else:
                face.play("curious"); face.update(); self.drain_face(face)
            before = (face.expression, face.caption, face.active_scene_sequence,
                      face.started, face.hold_ms, face.pose)
            self.assertFalse(face.release_scene(old))
            self.assertFalse(face.hold_scene(old))
            self.assertEqual((face.expression, face.caption, face.active_scene_sequence,
                              face.started, face.hold_ms, face.pose), before)

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
        self.assertIsNone(face.active_scene_sequence)
        face.update()
        self.drain_face(face)
        self.assertEqual(face.visible_scene_sequence, 2)
        self.assertEqual(face.caption, "")

if __name__ == "__main__":
    unittest.main()
