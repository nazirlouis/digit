"""Fault injection through the real parser/loop; movement feedback is independent."""
import unittest
import test_motion_jobs as jobs
from test_travel_motion import Encoders
from test_edge_retreat import Accelerometer


class RecoveryTests(unittest.TestCase):
    send = jobs.MotionTests.send
    step = jobs.MotionTests.step

    def setUp(self):
        jobs.MotionTests.setUp(self)
        self.feedback.encoders = Encoders()
        self.feedback.rate_target = lambda value: -1000 if value < 0 else 1000
        self.job.distance_scale = (10, 10)

    def travel(self, opt=False):
        self.send(1, "TRAVEL", "300 3000 6000 15000" + (" 1" if opt else ""))
        self.step(600)
        self.assertEqual(self.job.state, "RUNNING")

    def push(self, distance=300):
        self.send(1, "PUSH", str(distance))
        self.step(600)
        self.assertEqual(self.job.state, "RUNNING")

    def test_capability_and_push_argument_validation(self):
        self.uart.send("12 STATUS"); self.control.tick()
        self.assertTrue(any("RECOVERY=1 PUSH=1" in line for line in self.uart.outgoing))
        for arg in ("-1", "0", "9", "301", "30 6000", "1.5"):
            self.assertIn(" ERR ", self.send(1, "PUSH", arg))
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.job.seq, 0)

    def test_push_max_target_acceleration_and_correction_only_downward(self):
        self.tof.distance_mm = 20; self.step(25)
        self.push()
        self.assertEqual(self.control.acceleration, 30000)
        self.assertEqual(self.control.deceleration, 30000)
        self.step(5)
        self.assertEqual(self.motors.speeds, (6000, 6000))
        self.gyro.last_reading_dps = (0, 0, 20)
        self.step(20)
        self.assertTrue(all(5800 <= target <= 6000 for target in (self.control.target_left, self.control.target_right)))
        self.assertNotEqual(self.control.target_left, self.control.target_right)
        self.assertEqual(self.job.limit, 5000)

    def test_push_distance_includes_ramp_brake_and_actual_rest(self):
        self.push(100)
        self.feedback.encoders.counts = [1000, 1000]
        self.feedback.velocity = [200, 200]
        self.step(5)
        self.assertEqual(self.job.reason, "PUSH_BRAKING")
        self.assertEqual(self.control.deceleration, 30000)
        self.feedback.encoders.counts = [1020, 1020]
        self.step(400)
        self.assertEqual(self.job.state, "BRAKING")
        self.feedback.velocity = [0, 0]; self.step(200)
        self.assertEqual(self.job.reason, "PUSH_DONE")
        self.assertEqual(self.job.travelled_mm, 102)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_push_fast_stall_no_automatic_retry_and_normal_tof_restored(self):
        self.tof.distance_mm = 20; self.step(25); self.push()
        self.step(150)
        self.assertEqual(self.job.state, "RECOVERING")
        self.assertEqual(self.job.recovery_cause, "ENCODER_STALL")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.step(300)
        self.assertEqual(self.job.reason, "PUSH_BLOCKED")
        self.send(1, "PUSH", "300"); self.step(100)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("OBSTACLE", self.send(2, "TRAVEL", "100 3000 6000 15000"))

    def test_push_powered_limit_includes_braking_and_never_restarts(self):
        self.push()
        self.job.push_started = self.clock.now - 1995
        self.job.state = "BRAKING"
        self.control._stop(smooth=True)
        self.step(5)
        self.assertEqual(self.job.recovery_cause, "PUSH_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.step(300)
        self.assertEqual(self.job.reason, "PUSH_BLOCKED")

    def test_push_total_deadline_never_extends_for_stationary_recovery(self):
        self.push(); self.job.started = self.clock.now - 4995
        self.step(5)
        self.assertEqual(self.job.reason, "PUSH_DEADLINE")
        self.assertFalse(self.job.armed)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_push_edge_stop_and_manual_stop_cannot_leak_override(self):
        for stop in ("EDGE", "STOP", "heartbeat"):
            self.setUp(); self.push()
            if stop == "EDGE": self.sensors.values = [1024]*5
            elif stop == "STOP": self.uart.send("99 STOP")
            else: self.job.heartbeat = self.clock.now - 750
            self.step(25, heartbeat=False)
            if stop == "EDGE":
                self.assertTrue(self.job.armed)
                self.assertEqual(self.job.reason, "EDGE_BACK")
                self.assertLessEqual(self.motors.speeds[0], -3000)
                self.send(1, "PUSH", "300")
                self.step(300)
                self.assertLessEqual(self.motors.speeds[0], -3000)  # Never resumes the push.
                self.uart.send("99 STOP")
                self.step(5)
            self.assertFalse(self.job.armed)
            self.assertEqual(self.motors.speeds, (0, 0))
            self.send(1, "PUSH", "300")
            self.step(300)
            self.assertEqual(self.motors.speeds, (0, 0))

    def test_brief_invalid_tof_keeps_moving_and_expired_range_recovers(self):
        self.travel()
        original = self.job.started
        self.tof.range_status = 4; self.step(100)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertGreater(self.motors.speeds[0], 0)
        self.step(170)
        self.assertEqual(self.job.state, "RECOVERING")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.tof.range_status = 0
        self.step(300)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertEqual(self.job.started, original)
        self.assertEqual(self.job.recovery_count, 1)

    def test_recovery_needs_distinct_samples_and_tolerates_gyro_sample_intervals(self):
        self.travel(); self.tof.data_ready = False; self.step(270)
        self.gyro.data_ready = lambda: self.clock.now % 20 == 0
        self.step(200)
        self.assertEqual(self.job.state, "RECOVERING")
        self.tof.data_ready = True
        self.step(80)
        self.assertEqual(self.job.state, "RUNNING")

    def test_dropout_clearance_reduces_moving_grace(self):
        self.tof.distance_mm = 100; self.step(25); self.travel()
        self.feedback.velocity = [3000, 3000]
        self.tof.data_ready = False; self.step(70)
        self.assertEqual(self.job.state, "RECOVERING")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_fresh_beyond_range_keeps_drive_forward_until_numeric_obstacle(self):
        self.tof.range_status = 2; self.step(25)
        self.send(1, "DRIVE", "3000 3000 6000 50 10000"); self.step(1800)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertGreater(self.motors.speeds[0], 0)
        self.assertEqual(self.control.distance_mm, -1)
        self.assertEqual(self.job.recovery_count, 0)
        self.tof.range_status = 0; self.tof.distance_mm = 30; self.step(25)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.step(250)
        self.assertEqual(self.job.reason, "OBSTACLE_STOPPED")

    def test_expired_beyond_range_pauses_and_push_accepts_fresh_beyond_range(self):
        self.tof.range_status = 2; self.step(25); self.travel()
        self.tof.data_ready = False; self.step(270)
        self.assertEqual(self.job.state, "RECOVERING")
        self.setUp(); self.tof.range_status = 2; self.step(25); self.push()
        self.assertGreater(self.motors.speeds[0], 0)

    def test_recovery_deadline_and_one_nonblocking_restart(self):
        calls = []
        self.tof.stop_ranging = lambda: calls.append("stop")
        self.tof.restart_ranging_nonblocking = lambda: calls.append("start")
        self.travel(); self.tof.data_ready = False; self.step(2400)
        self.assertEqual(calls, ["stop", "start"])
        self.assertEqual(self.job.reason, "RECOVERY_FAILED")
        self.assertFalse(self.job.armed)
        self.assertLessEqual(len(self.job.reason), 23)

    def test_stop_during_recovery_never_resurrects(self):
        self.travel(); self.tof.data_ready = False; self.step(270)
        self.uart.send("99 STOP"); self.step(5)
        self.tof.data_ready = True; self.step(500)
        self.assertFalse(self.job.armed)
        self.assertFalse(self.job.recovery.active)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_control_gap_recovers_to_replan_without_invented_heading(self):
        self.travel(); self.clock.now += 160; self.step(5)
        self.assertEqual(self.job.state, "RECOVERING")
        self.assertFalse(self.job.position_known)
        self.step(250)
        self.assertEqual(self.job.reason, "BLOCKED_RECOVERED")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_motion_deadline_allows_only_stationary_settle_and_replan(self):
        self.travel(); self.job.limit = self.clock.now-self.job.started+5
        original = self.job.started
        self.step(5)
        self.assertEqual(self.job.state, "RECOVERING")
        self.assertEqual(self.job.recovery_cause, "JOB_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.step(250)
        self.assertEqual(self.job.reason, "BLOCKED_RECOVERED")
        self.assertEqual(self.job.started, original)

    def test_transient_encoder_io_does_not_discard_accumulated_counts(self):
        self.travel(); self.feedback.encoders.counts = [200, 200]; self.step(5)
        normal = self.feedback.encoders.get_counts
        self.feedback.encoders.get_counts = lambda: (_ for _ in ()).throw(OSError())
        self.step(5)
        self.assertEqual(self.job.state, "RECOVERING")
        self.feedback.encoders.counts = [230, 230]
        self.feedback.encoders.get_counts = normal; self.step(250)
        self.assertEqual(self.job.reason, "BLOCKED_RECOVERED")
        self.assertEqual(self.job.travelled_mm, 23)

    def test_encoder_speed_control_faults_enter_recovery_through_controller_tick(self):
        for reason in ("ENCODER_IO", "ENCODER_STALE"):
            self.setUp(); self.travel()
            normal = self.feedback.update
            self.feedback.update = lambda *args: (_ for _ in ()).throw(RuntimeError(reason))
            self.step(5)
            self.assertEqual(self.job.state, "RECOVERING")
            self.assertEqual(self.job.recovery_cause, reason)
            self.assertEqual(self.control.fault, "NONE")
            self.assertEqual(self.motors.speeds, (0, 0))
            self.feedback.update = normal; self.step(300)
            self.assertEqual(self.job.reason, "BLOCKED_RECOVERED")

    def test_encoder_speed_stall_uses_verified_backoff_through_controller_tick(self):
        self.control.accelerometer = self.control.edge_retreat.acc = Accelerometer()
        self.travel(opt=True)
        self.feedback.encoders.counts = [400, 400]
        self.feedback.velocity = [300, 300]; self.step(25)
        normal = self.feedback.update
        self.feedback.update = lambda *args: (_ for _ in ()).throw(RuntimeError("ENCODER_STALL"))
        self.step(5)
        self.assertEqual(self.job.reason, "BLOCK_WAIT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.feedback.update = normal
        self.feedback.velocity = [0, 0]; self.step(220)
        self.assertEqual(self.job.reason, "BLOCK_BACK")

    def test_persistent_encoder_io_locks_until_user_intervenes(self):
        self.travel()
        self.feedback.encoders.get_counts = lambda: (_ for _ in ()).throw(OSError())
        self.step(2050)
        self.assertEqual(self.control.fault, "ENCODER")
        self.assertFalse(self.control.ready)

    def test_push_sensor_recovery_never_resumes_interrupted_push(self):
        self.push(); self.tof.data_ready = False
        # Keep measured progress so this specifically exercises missing range.
        for _ in range(55):
            self.feedback.encoders.counts[0] += 2
            self.feedback.encoders.counts[1] += 2
            self.step(5)
        self.assertEqual(self.job.state, "RECOVERING")
        self.tof.data_ready = True; self.step(300)
        self.assertEqual(self.job.reason, "PUSH_BLOCKED")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_first_stall_only_retreats_on_verified_path(self):
        self.acc = Accelerometer(); self.control.accelerometer = self.control.edge_retreat.acc = self.acc
        self.travel(opt=True)
        self.feedback.encoders.counts = [400, 400]
        self.feedback.velocity = [300, 300]; self.step(25)
        self.job.cancel("ENCODER_STALL", True)
        self.assertEqual(self.job.reason, "BLOCK_WAIT")
        self.assertIn("ER=0 BACK=0", self.uart.outgoing[-1])
        self.feedback.velocity = [0, 0]; self.step(220)
        self.assertEqual(self.job.reason, "BLOCK_BACK")
        self.assertLess(self.motors.speeds[0], 0)
        self.feedback.encoders.counts = [290, 290]; self.step(10)
        self.assertEqual(self.job.reason, "BLOCK_SETTLE")
        self.step(250)
        self.assertEqual(self.job.reason, "BLOCKED_RECOVERED")
        self.assertIn("BACK=11", self.uart.outgoing[-1])
        self.assertTrue(self.control.ready)

    def test_stall_without_verified_path_never_blindly_reverses(self):
        self.travel(); self.job.cancel("ENCODER_STALL", True); self.step(250)
        self.assertEqual(self.control.fault, "STALL")
        self.assertFalse(self.control.ready)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_strong_orientation_guard_rejects_inversion_not_launch_acceleration(self):
        self.acc = Accelerometer(); self.control.accelerometer = self.acc
        self.travel()
        self.acc.last_reading_g = (1.8, 0, 1); self.step(300)
        self.assertTrue(self.control.ready)
        self.acc.last_reading_g = (0, 0, -1); self.step(250)
        self.assertEqual(self.control.fault, "ORIENTATION")
        self.assertFalse(self.job.armed)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.control.start_calibration("99"); self.step(650)
        self.assertFalse(self.control.ready)
        self.acc.last_reading_g = (0, 0, 1); self.step(250)
        self.control.start_calibration("100"); self.step(650)
        self.assertTrue(self.control.ready)

    def test_watchdog_lock_only_clears_after_explicit_successful_calibration(self):
        class Health:
            def __init__(self): self.cleared = 0
            def clear_after_user_calibration(self): self.cleared += 1
        self.send(0, "CANCEL")
        health = Health(); self.control.boot_health = health
        self.control.start_calibration(); self.step(650)
        self.assertEqual(health.cleared, 0)
        self.control.start_calibration("99"); self.step(650)
        self.assertEqual(health.cleared, 1)
        health.clear_after_user_calibration = lambda: (_ for _ in ()).throw(OSError())
        self.control.start_calibration("100"); self.step(650)
        self.assertFalse(self.control.ready)
        self.assertEqual(self.control.fault, "BOOT_HEALTH")


if __name__ == "__main__":
    unittest.main()
