"""Exercise gestures through the UART parser, display handshake and safety loop."""
import unittest
import time
from unittest.mock import patch
import test_uart_control as fixtures
from test_automatic_drive import RangeSensor
from test_motion_jobs import Feedback
from test_voice_turn import Gyro
from digit_face_engine import EXPRESSIONS
from expression_motion import SPEED
from motion_jobs import frame
from voice_turn import VoiceTurn


class GestureFace(fixtures.DeferredFace):
    def play(self, expression, hold_ms=None):
        super().play(expression, hold_ms)
        self.expression, self.returning = expression, False

    def show_scene(self, expression, caption, timing=None):
        self.play(expression)
        super().show_scene(expression, caption, timing)


class ExpressionMotionTests(unittest.TestCase):
    def setUp(self):
        self.f = fixtures.UARTControlTests()
        self.f.setUp()
        self.c = self.f.control
        self.face = GestureFace(self.f.clock)
        self.c.face = self.c.wake.face = self.face
        self.tof = self.c.tof = RangeSensor()
        self.f.calibrate()
        self.outputs = []
        original = self.f.motors.set_speeds

        def record(left, right):
            self.outputs.append((self.f.clock.now, left, right))
            original(left, right)
        self.f.motors.set_speeds = record

    def step(self, ms, host=True):
        self.f.advance(ms, "99 STATUS" if host else None)

    def request(self, name="HAPPY", reveal=True):
        self.assertEqual(self.f.command("2 SCENE " + name + " NONE 3000"), "2 OK SCENE")
        if reveal:
            self.face.reveal()
            self.step(5)

    def assert_stopped(self):
        self.assertFalse(self.c.expression_motion.active)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.assertEqual((self.c.target_left, self.c.target_right), (0, 0))

    def gesture_events(self):
        return [line for line in self.f.uart.outgoing if " EVENT GESTURE " in line]

    def test_chirp_events_follow_real_motor_start_and_stop_once(self):
        self.request(reveal=False)
        self.step(200)
        self.assertEqual(self.gesture_events(), [])
        self.face.reveal()
        self.step(5)
        self.assertEqual(self.f.motors.speeds, (-3000, 3000))
        self.assertEqual(self.gesture_events(), ["2 EVENT GESTURE START HAPPY"])
        self.step(70)  # A zero-power pause is not gesture completion.
        self.assertEqual(len(self.gesture_events()), 1)
        self.f.command("3 STOP")
        self.assert_stopped()
        self.assertEqual(self.gesture_events(), ["2 EVENT GESTURE START HAPPY", "2 EVENT GESTURE END HAPPY CANCEL"])
        self.step(2000)
        self.assertEqual(len(self.gesture_events()), 2)

    def test_chirp_events_on_completion_replacement_and_safety_stop(self):
        for outcome in ("done", "replacement", "edge", "timeout"):
            with self.subTest(outcome=outcome):
                self.setUp()
                self.request()
                if outcome == "replacement": self.f.command("3 SCENE NEUTRAL NONE")
                if outcome == "edge": self.f.sensors.values[0] = 980
                self.step(2200, host=outcome != "timeout")
                self.assert_stopped()
                reason = "DONE" if outcome == "done" else "CANCEL"
                self.assertEqual(self.gesture_events(), ["2 EVENT GESTURE START HAPPY", "2 EVENT GESTURE END HAPPY " + reason])

    def test_no_chirp_event_for_still_blocked_cancelled_or_unseen_gestures(self):
        for gate in ("NEUTRAL", "SERIOUS", "SLEEPING", "locked", "floor", "cancelled", "unseen"):
            with self.subTest(gate=gate):
                self.setUp()
                if gate == "locked": self.c.ready = False
                self.request(gate if gate.isupper() else "HAPPY", reveal=False)
                if gate == "floor": self.f.sensors.values[0] = 980
                if gate == "cancelled": self.f.command("3 STOP")
                if gate != "unseen": self.face.reveal()
                self.step(2200)
                self.assertEqual(self.gesture_events(), [])

    def test_all_thirteen_expressions_and_actual_output_floor(self):
        still = {"neutral", "serious", "sleeping"}
        for name in EXPRESSIONS:
            with self.subTest(expression=name):
                self.setUp()
                self.c.acceleration = 500  # Slider cannot swallow short gestures.
                self.request(name.upper())
                self.step(2200)
                self.assert_stopped()
                moving = [(t, l, r) for t, l, r in self.outputs if l or r]
                self.assertEqual(bool(moving), name not in still)
                for _, left, right in moving:
                    self.assertEqual((abs(left), abs(right)), (SPEED, SPEED))
                # Every polarity change passes through a stopped pause.
                for previous, current in zip(self.outputs, self.outputs[1:]):
                    self.assertFalse(previous[1] * current[1] < 0)
                    self.assertFalse(previous[2] * current[2] < 0)
                count = len(self.outputs)
                self.step(4000)
                self.assertEqual(len(self.outputs), count, "No idle replay")

    def test_waits_for_complete_visible_frame_then_plays_once(self):
        self.request(reveal=False)
        self.step(300)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.assertTrue(self.c.expression_motion.active)
        self.face.reveal()
        self.step(5)
        self.assertEqual(self.f.motors.speeds, (-3000, 3000))
        self.step(2200)
        self.assert_stopped()

    def test_unrendered_scene_times_out_without_late_start(self):
        self.request(reveal=False)
        self.step(1500)
        self.face.reveal()
        self.step(500)
        self.assert_stopped()
        self.assertFalse(self.outputs)

    def test_stop_and_brake_cancel_wait_pulse_and_pause(self):
        for command in ("STOP", "BRAKE"):
            for phase in ("waiting", "pulse", "pause"):
                with self.subTest(command=command, phase=phase):
                    self.setUp()
                    self.request(reveal=phase != "waiting")
                    if phase == "pause":
                        self.step(70)
                        self.assertEqual(self.f.motors.speeds, (0, 0))
                    self.assertEqual(self.f.command("3 " + command), "3 OK STOPPED")
                    self.assert_stopped()
                    self.face.reveal()
                    self.step(2500)
                    self.assert_stopped()

    def test_replacement_and_neutral_discard_old_remaining_steps(self):
        self.request()
        self.step(30)
        self.request("SAD", reveal=False)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.face.reveal()
        self.step(5)
        self.assertEqual(self.f.motors.speeds, (-3000, -3000))
        self.request("NEUTRAL")
        self.step(2000)
        self.assert_stopped()

    def test_short_scene_expiry_cancels_remaining_gesture(self):
        self.request("CRYING")
        self.step(100)
        self.face.returning = True
        self.step(5)
        self.assert_stopped()

    def test_locked_missing_range_and_busy_scenes_are_never_queued(self):
        for gate in ("uncalibrated", "fault", "calibration", "drive", "armed", "range", "button"):
            with self.subTest(gate=gate):
                self.setUp()
                if gate == "uncalibrated": self.c.ready = False
                if gate == "fault": self.c.fault = "SENSOR"
                if gate == "calibration": self.c.start_calibration()
                if gate == "drive": self.f.command("3 DRIVE 3000 3000 6000")
                if gate == "armed":
                    self.c.motion.armed = True
                    self.c.motion.heartbeat = self.f.clock.now
                if gate == "range": self.c.tof = None
                if gate == "button": self.f.c.pressed = True
                self.request()
                self.assertFalse(self.c.expression_motion.active)
                self.c._stop()
                self.c.motion.armed = False
                self.c.ready, self.c.fault, self.c.calibration = True, "NONE", None
                self.c.thresholds = [622] * 5
                self.c.tof, self.f.c.pressed = self.tof, False
                self.step(2200)
                self.assert_stopped()

    def test_manual_drive_owns_tracks_immediately_and_does_not_resume_gesture(self):
        self.request()
        self.f.command("3 DRIVE 4000 4000 6000")
        self.assertFalse(self.c.expression_motion.active)
        self.assertEqual((self.c.target_left, self.c.target_right), (4000, 4000))
        self.f.advance(700, "3 DRIVE 4000 4000 6000")
        self.assertEqual(self.f.motors.speeds, (4000, 4000))
        self.f.command("4 STOP")
        self.step(2000)
        self.assert_stopped()

    def test_new_autonomous_session_preempts_gesture(self):
        self.c.speed_control = Feedback(self.f.clock)
        self.c.voice_turn = VoiceTurn(Gyro(), self.f.clock)
        self.request()
        reply = self.f.command(frame("10", "M2 {} 1 0 ARM".format(self.c.motion.boot)))
        self.assertNotIn(" ERR ", reply)
        self.assertTrue(self.c.motion.armed)
        self.assert_stopped()

    def test_edge_button_sensor_encoder_faults_stop_without_replay(self):
        for failure in ("edge", "button", "sensor", "encoder"):
            with self.subTest(failure=failure):
                self.setUp()
                self.c.speed_control = Feedback(self.f.clock)
                self.request()
                if failure == "edge": self.f.sensors.values[0] = 980
                if failure == "button": self.f.c.event = True
                if failure == "sensor": self.f.sensors.fail = True
                if failure == "encoder": self.c.speed_control.fail = True
                self.step(20)
                self.assert_stopped()
                self.assertFalse(self.c.ready)

    def test_floor_is_checked_again_before_first_pulse(self):
        self.request(reveal=False)
        self.face.reveal()
        self.f.sensors.values[0] = 900
        self.step(5)  # Before the regular 20 ms poll.
        self.assert_stopped()
        self.assertEqual(self.c.fault, "EDGE")
        self.assertFalse(self.outputs)

    def test_front_clearance_and_range_freshness(self):
        self.tof.distance_mm = 49
        self.step(20)
        self.request()
        self.assert_stopped()
        self.request("SAD")  # Backward withdrawal can clear a front obstacle.
        self.assertEqual(self.f.motors.speeds, (-3000, -3000))
        self.request("NEUTRAL")
        self.tof.distance_mm = 900
        self.step(20)
        self.request("LOVE")
        self.tof.data_ready = False
        self.step(260)
        self.assert_stopped()

    def test_host_loss_and_slow_loop_cancel_pulses_and_pauses(self):
        self.request("SLEEPY")
        self.step(510, host=False)
        self.assert_stopped()
        self.request()
        self.f.clock.now += 51
        self.c.tick()
        self.assert_stopped()
        self.step(2000)
        self.assert_stopped()

    def test_feedback_cannot_boost_gesture_output(self):
        feedback = Feedback(self.f.clock)
        feedback.update = lambda now, targets, *args: tuple(
            6000 if v > 0 else -6000 if v < 0 else 0 for v in targets)
        self.c.speed_control = feedback
        self.request()
        self.step(2200)
        self.assert_stopped()
        self.assertTrue(any(l or r for _, l, r in self.outputs))
        self.assertTrue(all(abs(v) in (0, 3000) for _, l, r in self.outputs for v in (l, r)))

    def test_legacy_face_and_buttons_use_same_visibility_gated_gestures(self):
        self.assertEqual(self.f.command("3 FACE HAPPY"), "3 OK FACE")
        self.face.reveal()
        self.step(5)
        self.assertEqual(self.f.motors.speeds, (-3000, 3000))
        self.f.command("4 STOP")
        for button, expected in ((self.f.a, (-3000, -3000)), (self.f.b, (-3000, 3000))):
            button.event = True
            self.c.tick()
            self.face.reveal()
            self.step(5, host=False)
            self.assertEqual(self.f.motors.speeds, expected)
            self.step(1800, host=False)
            self.assert_stopped()

    def test_real_cooperative_renderer_and_short_scene_expiry(self):
        from digit_face_engine import FaceEngine
        from test_face_engine import CooperativeDisplay
        with patch.multiple(time, ticks_ms=self.f.clock.ticks_ms,
                            ticks_us=lambda: self.f.clock.now * 1000,
                            ticks_diff=self.f.clock.ticks_diff,
                            ticks_add=self.f.clock.ticks_add, create=True):
            display = CooperativeDisplay()
            face = FaceEngine(display)
            face.enable_cooperative()
            self.c.face = self.c.wake.face = face
            self.f.command("2 SCENE EXCITED NONE 500")
            self.assertEqual(self.f.motors.speeds, (0, 0))
            for _ in range(100):
                if face.visible_scene_sequence == face.active_scene_sequence:
                    break
                self.assertEqual(self.f.motors.speeds, (0, 0))
                self.step(5)
            self.assertEqual(face.visible_scene_sequence, face.active_scene_sequence)
            self.step(5)
            self.assertEqual(self.f.motors.speeds, (-3000, 3000))
            self.step(750)
            self.assert_stopped()


if __name__ == "__main__":
    unittest.main()
