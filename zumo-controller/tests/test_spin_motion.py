"""One continuous revolution through the production M2 parser/safety loop."""
import unittest
import time
from unittest.mock import patch
import test_motion_jobs as motion_fixtures
from encoder_speed import EncoderSpeed
from voice_turn import SPIN_ACCELERATION
from digit_face_engine import FaceEngine
from reaction_catalog import TIMINGS
from test_face_engine import CooperativeDisplay


class SpinTests(unittest.TestCase):
    setUp = motion_fixtures.MotionTests.setUp
    send = motion_fixtures.MotionTests.send
    step = motion_fixtures.MotionTests.step

    def start_spin(self, deadline=12000):
        reply = self.send(1, "SPIN", str(deadline))
        self.assertNotIn(" ERR ", reply)
        self.assertEqual(self.job.state, "CALIBRATING")

    def simulate(self, duration, motor_sign=1, encoder_velocity=True):
        for _ in range(duration // 5):
            left, right = self.motors.speeds
            self.gyro.last_reading_dps = (0, 0, (right-left) * motor_sign / 30)
            if encoder_velocity:
                self.feedback.velocity = [left / 2, right / 2]
            self.step(5)

    def test_single_continuous_unwrapped_revolution_with_signed_opposite_tracks(self):
        for motor_sign in (1, -1):
            with self.subTest(motor_sign=motor_sign):
                self.setUp(); self.control.voice_turn.motor_sign = motor_sign
                self.job.heading = 27; self.start_spin()
                started_moving = stopped = False
                starts = peak = 0
                for _ in range(1600):
                    self.simulate(5, motor_sign)
                    left, right = self.motors.speeds
                    peak = max(peak, abs(left), abs(right))
                    self.assertEqual(left, -right)
                    self.assertIsNone(self.control.track_speed_ratios)
                    if left or right:
                        self.assertFalse(stopped, "spin must not restart after rest")
                        if not started_moving: starts += 1
                        started_moving = True
                        self.assertEqual((left > 0, right > 0), (motor_sign < 0, motor_sign > 0))
                    elif started_moving:
                        stopped = True
                    if not self.job.active: break
                self.assertEqual(starts, 1)
                self.assertEqual(peak, 6000)
                self.assertEqual((self.job.state, self.job.reason), ("DONE", "SPIN_DONE"))
                self.assertGreaterEqual(self.job.heading - self.job.turn_origin, 360)
                self.assertLess(self.job.heading - self.job.turn_origin, 380)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertEqual(self.control.acceleration, SPIN_ACCELERATION)

    def test_encoder_confirmed_rest_is_required_after_full_rotation(self):
        self.start_spin(); self.step(410)
        self.feedback.velocity = [-400, 400]
        self.simulate(3000, encoder_velocity=False)
        self.assertEqual(self.job.state, "SETTLING")
        self.assertGreaterEqual(self.job.heading - self.job.turn_origin, 360)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertNotEqual(self.job.reason, "SPIN_DONE")
        self.feedback.velocity = [0, 0]; self.simulate(160)
        self.assertEqual((self.job.state, self.job.reason), ("DONE", "SPIN_DONE"))

    def test_exact_retry_and_terminal_retry_cannot_restart_spin(self):
        self.start_spin(); self.simulate(900)
        started, bias, angle = self.job.started, self.control.voice_turn.bias, self.control.voice_turn.angle
        self.send(1, "SPIN", "12000")
        self.assertEqual(self.job.started, started)
        self.assertEqual(self.control.voice_turn.bias, bias)
        self.assertEqual(self.control.voice_turn.angle, angle)
        self.assertIn("ERR M2_SEQ", self.send(1, "SPIN", "13000"))
        self.simulate(3000); heading = self.job.heading
        self.send(1, "SPIN", "12000"); self.simulate(500)
        self.assertEqual(self.job.heading, heading)
        self.assertEqual(self.job.reason, "SPIN_DONE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_argument_limits_legacy_turn_limit_and_capability(self):
        for args in ("", "999", "30001", "360 12000", "-12000", "12000 extra"):
            self.assertIn("ERR M2_", self.send(1, "SPIN", args))
            self.assertEqual(self.job.seq, 0)
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("ERR M2_ARGUMENT", self.send(1, "TURN", "360 6000 12000"))
        self.uart.send("99 STATUS"); self.control.tick()
        self.assertTrue(any("SPIN=1" in line for line in self.uart.outgoing))

    def test_at_five_cm_and_fresh_no_return_complete(self):
        for status in (0, 2):
            with self.subTest(status=status):
                self.setUp(); self.tof.range_status = status
                self.tof.distance_mm = 50 if status == 0 else 24
                self.step(25); self.start_spin(); self.simulate(4000)
                self.assertEqual(self.job.reason, "SPIN_DONE")
                self.assertEqual(self.control.distance_mm, 50 if status == 0 else -1)

    def test_brief_range_noise_preserves_obstacle_guard(self):
        self.start_spin(); self.simulate(900)
        for status in (2, 1, 4, 7, 12):
            self.tof.range_status = status; self.simulate(40)
            self.assertTrue(self.job.active)
            self.tof.range_status = 0; self.simulate(40)
        self.tof.distance_mm = 49; self.simulate(25)
        self.assertEqual(self.job.reason, "OBSTACLE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_range_failures_cannot_use_no_return(self):
        for failure in ("stale", "bus", 1, 3, 4, 5, 12, 255):
            with self.subTest(failure=failure):
                self.setUp(); self.tof.range_status = 2
                self.step(25); self.start_spin(); self.simulate(850)
                if failure == "stale": self.tof.data_ready = False
                elif failure == "bus":
                    def broken_read(): raise OSError("I2C disconnected")
                    self.tof.read_result = broken_read
                else: self.tof.range_status = failure
                self.simulate(300)
                self.assertEqual(self.job.state, "RECOVERING")
                self.simulate(2100)
                self.assertEqual(self.job.state, "FAULT")
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_stop_cancel_takeover_calibration_and_heartbeat_cannot_replay(self):
        for command in ("99 STOP", "99 CALIBRATE", "99 DRIVE 3000 3000 6000", "CANCEL", "heartbeat"):
            with self.subTest(command=command):
                self.setUp(); self.start_spin(); self.simulate(850)
                if command == "CANCEL": self.send(0, "CANCEL")
                elif command == "heartbeat": self.step(760, heartbeat=False)
                else:
                    self.uart.send(command); self.control.tick()
                self.assertFalse(self.job.armed)
                self.assertFalse(self.job.active)
                self.send(1, "SPIN", "12000")
                self.assertNotEqual(self.job.kind, "SPIN")
                if "DRIVE" not in command: self.assertEqual(self.motors.speeds, (0, 0))

    def test_edge_button_encoder_gyro_and_control_gap_stop(self):
        for failure in ("edge", "button", "encoder", "gyro", "gyro_stale", "gap"):
            with self.subTest(failure=failure):
                self.setUp(); self.start_spin(); self.simulate(850)
                if failure == "edge": self.sensors.values[0] = 950
                elif failure == "button": self.c.pressed = self.c.event = True
                elif failure == "encoder": self.feedback.fail = True
                elif failure == "gyro": self.gyro.fail = True
                elif failure == "gyro_stale": self.gyro.ready = False
                else: self.clock.now += 151
                self.simulate(170 if failure == "gyro_stale" else 25)
                if failure in ("gyro", "gyro_stale", "gap"):
                    self.assertEqual(self.job.state, "RECOVERING")
                else:
                    self.assertFalse(self.job.active)
                    self.assertEqual(self.job.state, "FAULT")
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_direction_stall_deadline_and_stationary_calibration(self):
        for failure in ("direction", "stall", "deadline", "calibration_motion", "calibration_noise"):
            with self.subTest(failure=failure):
                self.setUp(); self.start_spin(1000 if failure == "deadline" else 12000)
                if failure == "calibration_motion":
                    self.feedback.velocity = [100, 0]; self.step(5)
                    self.assertEqual(self.job.reason, "CALIBRATION_MOVING")
                elif failure == "calibration_noise":
                    for n in range(82):
                        self.gyro.last_reading_dps = (0, 0, 20 if n % 2 else -20); self.step(5)
                    self.assertEqual(self.job.reason, "GYRO_UNSTABLE")
                elif failure == "direction":
                    self.step(450); self.gyro.last_reading_dps = (0, 0, -100); self.step(100)
                    self.assertEqual(self.job.reason, "SPIN_DIRECTION")
                elif failure == "stall":
                    self.step(1600); self.assertEqual(self.job.reason, "SPIN_STALLED")
                else:
                    self.simulate(1100); self.assertEqual(self.job.reason, "JOB_TIMEOUT")
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertNotEqual(self.job.reason, "SPIN_DONE")

    def test_real_encoder_controller_tracks_and_settles_spin(self):
        class Encoders:
            def __init__(self): self.counts = [0.0, 0.0]
            def get_counts(self): return tuple(int(v) for v in self.counts)
        encoders = Encoders()
        self.control.speed_control = EncoderSpeed(encoders, self.clock, {
            "version": 1, "low_cps": [1500, 1500], "high_cps": [3000, 3000],
            "reverse_low_cps": [1500, 1500], "reverse_high_cps": [3000, 3000]})
        self.start_spin()
        for _ in range(1600):
            left, right = self.motors.speeds
            encoders.counts[0] += left * .5 * .005
            encoders.counts[1] += right * .5 * .005
            self.gyro.last_reading_dps = (0, 0, (right-left) / 30)
            self.step(5)
            self.assertEqual(self.control.target_left, -self.control.target_right)
            self.assertEqual(self.motors.speeds[0], -self.motors.speeds[1])
            if not self.job.active: break
        self.assertEqual((self.job.state, self.job.reason), ("DONE", "SPIN_DONE"))
        self.assertGreaterEqual(self.job.heading-self.job.turn_origin, 360)
        self.assertTrue(all(abs(v) <= 50 for v in self.control.speed_control.velocity))

    def test_abnormal_braking_or_settling_yaw_faults_above_one_revolution(self):
        for phase in ("braking", "settling"):
            with self.subTest(phase=phase):
                self.setUp(); self.start_spin()
                for _ in range(1200):
                    self.simulate(5)
                    reached = (self.control.voice_turn.spin_braking if phase == "braking"
                               else self.job.state == "SETTLING")
                    if reached: break
                self.assertTrue(reached)
                self.assertGreaterEqual(self.job.heading-self.job.turn_origin, 360)
                self.gyro.last_reading_dps = (0, 0, 500)
                self.step(100)
                self.assertEqual((self.job.state, self.job.reason), ("FAULT", "SPIN_OVERSHOOT"))
                self.assertEqual(self.motors.speeds, (0, 0))
                self.send(1, "SPIN", "12000"); self.step(300)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertNotEqual(self.job.reason, "SPIN_DONE")

    def test_final_encoder_rest_requires_gyro_to_remain_quiet(self):
        self.start_spin()
        for _ in range(1200):
            self.simulate(5)
            if self.job.state == "SETTLING": break
        self.assertEqual(self.job.state, "SETTLING")
        self.gyro.last_reading_dps = (0, 0, 20); self.step(200)
        self.assertEqual(self.job.state, "SETTLING")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.gyro.last_reading_dps = (0, 0, 0); self.step(160)
        self.assertEqual((self.job.state, self.job.reason), ("DONE", "SPIN_DONE"))


class SpinFaceTests(unittest.TestCase):
    send = motion_fixtures.MotionTests.send
    step = motion_fixtures.MotionTests.step
    start_spin = SpinTests.start_spin

    def setUp(self):
        motion_fixtures.MotionTests.setUp(self)
        for item in (
            patch.object(time, "ticks_ms", lambda: self.clock.now, create=True),
            patch.object(time, "ticks_us", lambda: self.clock.now * 1000, create=True),
            patch.object(time, "ticks_add", self.clock.ticks_add, create=True),
            patch.object(time, "ticks_diff", self.clock.ticks_diff, create=True),
        ):
            item.start(); self.addCleanup(item.stop)
        self.face = FaceEngine(CooperativeDisplay())
        self.face.enable_cooperative()
        self.control.face = self.control.wake.face = self.face

    def scene(self, expression="HAPPY", caption="FOUND"):
        self.uart.send("99 SCENE {} {}".format(expression, caption)); self.control.tick()
        sequence = self.face.scene_sequence
        for _ in range(100):
            if self.face.visible_scene_sequence == sequence: return sequence
            self.step(5)
        self.fail("scene did not become visible")

    def simulate(self, duration):
        for _ in range(duration // 5):
            left, right = self.motors.speeds
            self.gyro.last_reading_dps = (0, 0, (right-left) / 60)
            self.feedback.velocity = [left / 2, right / 2]
            self.step(5)

    def test_real_happy_scene_stays_through_spin_then_releases_on_done(self):
        sequence = self.scene(); started = self.face.started
        self.start_spin()
        self.assertEqual(self.job.spin_scene, sequence)
        self.assertEqual(self.face.started, started)
        self.assertEqual(self.face.scene_sequence, sequence)
        self.simulate(2000)
        self.assertTrue(self.job.active)
        self.assertGreater(self.clock.now-started, sum(TIMINGS["happy"]))
        self.assertEqual((self.face.expression, self.face.caption), ("happy", "FOUND"))
        self.assertIsNone(self.face.hold_ms)
        self.simulate(2500)
        self.assertEqual(self.job.reason, "SPIN_DONE")
        self.assertIsNone(self.job.spin_scene)
        self.assertIsNone(self.face._held_scene_sequence)
        self.assertEqual(self.face.scene_sequence, sequence)
        self.step(sum(TIMINGS["happy"]) + 30)
        self.assertEqual(self.face.expression, "neutral")

    def test_stop_cancel_fault_and_manual_takeover_release_real_face_hold(self):
        for action in ("stop", "cancel", "edge", "heartbeat", "manual"):
            with self.subTest(action=action):
                self.setUp(); self.scene(); self.start_spin(); self.simulate(900)
                if action == "stop":
                    self.uart.send("99 STOP"); self.control.tick()
                elif action == "cancel": self.send(0, "CANCEL")
                elif action == "edge":
                    self.sensors.values[0] = 950; self.step(25)
                elif action == "heartbeat": self.step(760, heartbeat=False)
                else:
                    self.uart.send("99 DRIVE 3000 3000 6000"); self.control.tick()
                self.assertFalse(self.job.active)
                self.assertIsNone(self.job.spin_scene)
                self.assertIsNone(self.face._held_scene_sequence)
                if action == "stop":
                    self.assertEqual(self.face.expression, "neutral")
                else:
                    self.assertIsNotNone(self.face.hold_ms)

    def test_replacement_scene_keeps_ownership_after_old_spin_completion(self):
        old = self.scene(); self.start_spin(); self.simulate(900)
        replacement = self.scene("SLEEPING", "BATTERY")
        self.assertNotEqual(old, replacement)
        self.assertIsNone(self.face._held_scene_sequence)
        started = self.face.started
        self.simulate(3500)
        self.assertEqual(self.job.reason, "SPIN_DONE")
        self.assertEqual((self.face.active_scene_sequence, self.face.visible_scene_sequence),
                         (replacement, replacement))
        self.assertEqual((self.face.expression, self.face.caption), ("sleeping", "BATTERY"))
        self.assertEqual(self.face.started, started)

    def test_plain_spin_does_not_hold_or_select_an_unrelated_face(self):
        for expression, caption in (("HAPPY", "BATTERY"), ("SAD", "FOUND"), ("NEUTRAL", "NONE")):
            with self.subTest(expression=expression, caption=caption):
                self.setUp(); sequence = self.scene(expression, caption); self.start_spin()
                self.assertIsNone(self.job.spin_scene)
                self.assertIsNone(self.face._held_scene_sequence)
                self.assertEqual(self.face.scene_sequence, sequence)
                self.assertEqual(self.face.expression, expression.lower())


if __name__ == "__main__":
    unittest.main()
