"""Exercise edge events through the real UART, safety loop and motion job.

Encoder/IMU inputs are independent of commanded motors: no simulated success
is inferred merely because a target was requested.
"""
import unittest
import math
import test_motion_jobs as jobs
from test_travel_motion import Encoders


class Accelerometer:
    def __init__(self):
        self.ready, self.fail = True, False
        self.last_reading_g = (0, 0, 1)
    def data_ready(self):
        return self.ready
    def read(self):
        if self.fail:
            raise OSError("acc disconnected")


class EdgeRetreatTests(unittest.TestCase):
    send = jobs.MotionTests.send
    step = jobs.MotionTests.step

    def setUp(self):
        jobs.MotionTests.setUp(self)
        self.acc = Accelerometer()
        self.retreat = self.control.edge_retreat
        self.retreat.acc = self.acc
        self.control.accelerometer = self.acc
        self.feedback.encoders = Encoders()
        self.feedback.rate_target = lambda value: -1000 if value < 0 else 1000
        self.job.distance_scale = (10, 10)

    def forward(self, opt_in=True, distance=40):
        self.args = "100 3000 6000 15000" + (" 1" if opt_in else "")
        self.send(1, "TRAVEL", self.args)
        self.step(600)
        self.feedback.velocity = [300, 300]
        self.feedback.encoders.counts = [distance*10, distance*10]
        self.step(25)

    def edge(self):
        self.sensors.values = [1024, 100, 100, 100, 100]
        self.step(20)

    def back(self):
        self.forward()
        self.thresholds = self.control.thresholds[:]
        self.edge()
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertFalse(self.control.ready)
        self.assertLessEqual(self.motors.speeds[0], -3000)
        self.assertLessEqual(self.motors.speeds[1], -3000)

    def clear_and_settle(self):
        self.feedback.encoders.counts = [300, 300]
        self.feedback.velocity = [-350, -343]
        self.step(25)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.feedback.encoders.counts = [-600, -600]
        self.step(10)
        self.assertEqual(self.job.reason, "EDGE_SETTLE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.step(100)
        self.assertNotEqual(self.job.state, "DONE")
        self.feedback.velocity = [0, 0]
        self.step(250)

    def test_edge_commits_immediately_to_3000_without_imu_or_encoder_rest_wait(self):
        self.forward()
        self.feedback.velocity = [1864, 1856]
        self.gyro.last_reading_dps = (30, 0, 0)
        self.acc.last_reading_g = (.94, 0, .342)
        self.edge()
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertTrue(self.job.armed)
        self.assertLessEqual(self.motors.speeds[0], -3000)
        self.assertLessEqual(self.motors.speeds[1], -3000)
        self.assertIsNone(self.control.track_speed_ratios)

    def test_reported_7mm_imu_abort_no_longer_cancels_direct_reverse(self):
        self.back()
        self.feedback.encoders.counts = [330, 330]
        self.feedback.velocity = [-350, -343]
        self.gyro.last_reading_dps = (30, 0, 0)
        self.acc.last_reading_g = (.94, 0, .342)
        self.step(25)
        self.assertEqual(self.retreat.back_mm, 7)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertLessEqual(self.motors.speeds[0], -3000)
        self.feedback.encoders.counts = [-600, -600]
        self.step(10)
        self.feedback.velocity = [0, 0]
        self.step(220)
        self.assertEqual(self.job.reason, "EDGE_RETREATED")
        self.assertEqual(self.retreat.back_mm, 100)

    def test_backward_encoder_feedback_is_measurement_not_ground_displacement(self):
        self.back()
        self.step(300)
        self.assertEqual(self.retreat.back_mm, 0)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertFalse(any(" DONE EDGE_RETREATED " in text for text in self.uart.outgoing))
        self.step(750)
        self.assertEqual(self.job.reason, "EDGE_STALL")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_forward_braking_drift_is_not_a_failed_reverse(self):
        self.back()
        self.feedback.encoders.counts = [429, 427]
        self.feedback.velocity = [1864, 1856]
        self.step(20)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertLess(self.retreat.back_mm, 0)
        self.assertNotIn("BACK=-", self.send(1, "TRAVEL", self.args))
        self.feedback.encoders.counts = [-600, -600]
        self.feedback.velocity = [0, 0]
        self.step(240)
        self.assertEqual(self.job.reason, "EDGE_RETREATED")
        self.assertEqual(self.retreat.back_mm, 100)
        self.assertEqual(self.job.travelled_mm, -60)

    def test_full_reverse_ignores_repeated_worsening_and_uncleared_edge_readings(self):
        self.back()
        self.sensors.values = [1024]*5
        self.feedback.encoders.counts = [330, 330]
        self.step(25)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.clear_and_settle()
        self.assertEqual(self.job.reason, "EDGE_RETREATED")
        self.assertTrue(self.control.ready)
        self.assertEqual(self.control.thresholds, self.thresholds)

    def test_missing_stale_invalid_tilted_or_failed_imu_cannot_block_reverse(self):
        for fault in ("missing", "stale", "invalid", "tilt", "gyro_io", "acc_io"):
            with self.subTest(fault=fault):
                self.setUp(); self.forward()
                if fault == "missing":
                    self.control.accelerometer = self.retreat.acc = None
                if fault == "stale":
                    self.gyro.ready = self.acc.ready = False
                    self.control.accel_at = self.clock.now-101
                if fault == "invalid":
                    self.acc.last_reading_g = (float("nan"), 0, 0)
                if fault == "tilt":
                    self.acc.last_reading_g = (.985, 0, .174)
                if fault == "gyro_io": self.gyro.fail = True
                if fault == "acc_io": self.acc.fail = True
                self.edge()
                self.assertEqual(self.job.reason, "EDGE_BACK")
                self.assertLessEqual(self.motors.speeds[0], -3000)
                self.clear_and_settle()
                self.assertEqual(self.job.reason, "EDGE_RETREATED")

    def test_front_range_and_floor_read_failures_do_not_interrupt_committed_reverse(self):
        self.back()
        self.tof.data_ready = False
        self.sensors.fail = True
        self.step(300)
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertLessEqual(self.motors.speeds[0], -3000)
        self.sensors.fail = False
        self.clear_and_settle()
        self.assertEqual(self.job.reason, "EDGE_RETREATED")

    def test_startup_calibration_failure_cannot_start_motion(self):
        self.send(0, "CANCEL")
        self.control.thresholds = None
        self.control.ready = False
        self.control.fault = "CAL_FAILED"
        self.uart.send("30 DRIVE 3000 3000")
        self.step(10)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_small_travel_and_no_per_job_permission_still_reverse(self):
        for permit, distance in ((False, 40), (True, 20)):
            with self.subTest(permit=permit, distance=distance):
                self.setUp(); self.forward(permit, distance); self.edge()
                self.assertEqual(self.job.reason, "EDGE_BACK")
                self.assertTrue(self.job.armed)
                self.assertLessEqual(self.motors.speeds[0], -3000)

    def test_stop_button_manual_takeover_lease_and_control_deadline_preempt_reverse(self):
        for reason in ("STOP", "BRAKE", "BUTTON_C", "DRIVE", "HEARTBEAT", "CONTROL", "DEADLINE"):
            with self.subTest(reason=reason):
                self.setUp(); self.back()
                if reason in ("STOP", "BRAKE"): self.uart.send("70 "+reason)
                if reason == "BUTTON_C": self.c.pressed = True
                if reason == "DRIVE": self.uart.send("70 DRIVE 3000 3000")
                if reason == "HEARTBEAT": self.job.heartbeat = self.clock.now-751
                if reason == "CONTROL": self.clock.now += 151
                if reason == "DEADLINE": self.job.limit = self.clock.now-self.job.started
                self.step(10, heartbeat=False)
                self.assertFalse(self.job.armed)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertIn("DISARMED", self.send(1, "TRAVEL", self.args))

    def test_stop_queued_with_edge_prevents_any_applied_reverse_output(self):
        self.forward()
        outputs = []
        self.motors.set_speeds = lambda l, r: outputs.append((l, r))
        self.uart.send("70 STOP")
        self.edge()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertFalse(any(l<0 or r<0 for l, r in outputs))
        self.assertFalse(self.job.armed)

    def test_stopped_encoders_and_recovery_deadline_are_still_required(self):
        self.back()
        self.feedback.encoders.counts = [-600, -600]
        self.feedback.velocity = [-350, -343]
        self.step(10)
        self.assertEqual(self.job.reason, "EDGE_SETTLE")
        self.step(2500)
        self.assertEqual(self.job.reason, "EDGE_NOT_STOPPED")
        self.assertFalse(self.control.ready)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("cps:-350,-343", self.retreat.reject_detail)

    def test_same_completed_edge_episode_does_not_cancel_result_or_repeat_reverse(self):
        self.back(); self.clear_and_settle()
        self.assertEqual(self.job.reason, "EDGE_RETREATED")
        self.step(300)
        self.assertTrue(self.control.ready)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("ER=1 BACK=100", self.uart.outgoing[-1])
        self.sensors.values = [100]*5
        self.step(25)
        self.assertEqual(self.control.edge_release_mask, 0)
        self.sensors.values[0] = 1024
        self.step(25)
        self.assertFalse(self.control.ready)  # A newly detected idle edge is a new episode.

    def test_same_job_retry_never_restarts_reverse_or_resumes_forward(self):
        self.back()
        identity = self.job.started, self.job.session, self.job.seq
        self.send(1, "TRAVEL", self.args)
        self.assertEqual((self.job.started, self.job.session, self.job.seq), identity)
        self.assertIn("BUSY", self.send(2, "TURN", "30 6000 10000"))
        self.clear_and_settle()
        self.send(1, "TRAVEL", self.args)
        self.step(25)
        self.assertEqual(self.motors.speeds, (0, 0))

    def manual_forward(self):
        self.send(0, "CANCEL")
        for _ in range(5):
            self.uart.send("31 STATUS")
            self.uart.send("30 DRIVE 3000 3000 6000")
            self.step(100, heartbeat=False)
        self.feedback.velocity = [3637, 3637]
        self.sensors.values[0] = 1024
        self.step(20, heartbeat=False)
        reverse = self.control.edge_reverse
        self.assertTrue(reverse.active and reverse.automatic)
        self.assertEqual(reverse.state, "BACK")
        self.assertLessEqual(self.motors.speeds[0], -3000)
        return reverse

    def test_dashboard_automatic_reverse_ignores_imu_and_held_forward_and_finishes_100mm(self):
        reverse = self.manual_forward()
        self.acc.last_reading_g = (.94, 0, .342)
        self.gyro.last_reading_dps = (30, 0, 0)
        self.feedback.encoders.counts = [-70, -70]
        self.feedback.velocity = [-350, -343]
        self.sensors.values = [1024]*5
        self.uart.send("32 DRIVE 6000 6000 6000")
        self.step(25, heartbeat=False)
        self.assertTrue(reverse.active)
        self.assertEqual(reverse.back_mm, 7)
        self.assertEqual((self.control.target_left, self.control.target_right), (-3000, -3000))
        self.feedback.encoders.counts = [-1000, -1000]
        self.step(10, heartbeat=False)
        self.assertEqual(reverse.state, "SETTLE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.feedback.velocity = [0, 0]
        for _ in range(50):
            self.uart.send("31 STATUS")
            self.step(5, heartbeat=False)
        self.assertTrue(self.control.ready)
        self.assertEqual(reverse.back_mm, 100)

    def test_dashboard_stop_and_host_loss_cancel_auto_reverse(self):
        for stop in (True, False):
            with self.subTest(stop=stop):
                self.setUp(); reverse = self.manual_forward()
                if stop:
                    self.uart.send("32 STOP"); self.step(5, heartbeat=False)
                else:
                    self.step(760, heartbeat=False)
                    self.assertEqual(self.control.fault, "EDGE_HOST_TIMEOUT")
                self.assertFalse(reverse.active)
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_turn_edge_uses_same_immediate_reverse(self):
        self.send(1, "TURN", "30 6000 10000")
        self.step(600)
        self.edge()
        self.assertEqual(self.job.reason, "EDGE_BACK")
        self.assertLessEqual(self.motors.speeds[0], -3000)

    def test_ordinary_orientation_and_floor_color_checks_remain(self):
        self.forward()
        self.sensors.values = [800]*5
        self.step(150)
        self.assertTrue(self.control.ready)
        self.acc.last_reading_g = (.985, 0, .174)
        self.step(220)
        self.assertEqual(self.control.fault, "ORIENTATION")
        self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
