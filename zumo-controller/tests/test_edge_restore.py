"""A lifted robot can regain readiness without changing its floor baseline."""
import unittest
import test_uart_control as fixtures
import test_motion_jobs as motion_fixtures
from uart_control import EDGE_CLEAR_MS, EDGE_CLEAR_MARGIN, EDGE_POLL_MS


class EdgeRestoreTests(unittest.TestCase):
    def setUp(self):
        self.f = fixtures.UARTControlTests()
        self.f.setUp()
        self.f.calibrate()
        self.control = self.f.control
        self.thresholds = self.control.thresholds[:]

    def lift(self):
        self.f.sensors.values = [1024] * 5
        self.f.advance(EDGE_POLL_MS)
        self.assertEqual(self.control.fault, "EDGE")
        self.assertFalse(self.control.ready)

    def put_down(self):
        self.f.sensors.values = [100] * 5
        self.f.advance(EDGE_CLEAR_MS + EDGE_POLL_MS)

    def test_lift_and_replace_reuses_baseline_but_never_restarts_drive(self):
        self.f.command("2 DRIVE 1000 1000")
        self.f.advance(100)
        self.assertNotEqual(self.f.motors.speeds, (0, 0))
        self.lift()
        self.f.advance(2000)
        self.assertFalse(self.control.ready)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.put_down()
        self.assertTrue(self.control.ready)
        self.assertFalse(self.control.edge)
        self.assertEqual(self.control.fault, "NONE")
        self.assertEqual(self.control.thresholds, self.thresholds)
        self.assertIsNone(self.control.last_drive)
        self.assertIsNone(self.control.calibration)
        self.f.advance(2000)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.assertEqual((self.control.target_left, self.control.target_right), (0, 0))
        self.assertEqual(self.f.uart.outgoing.count("0 EVENT READY"), 2)
        self.assertIn("READY=1 FAULT=NONE", self.f.command("3 STATUS"))
        self.assertEqual(self.f.command("4 DRIVE 500 500"), "4 OK DRIVE")

    def test_all_five_sensors_need_a_full_second_and_clearance_margin(self):
        for sensor in range(5):
            with self.subTest(sensor=sensor):
                self.setUp()
                self.lift()
                self.f.sensors.values = [100] * 5
                self.f.advance(800)
                self.assertFalse(self.control.ready)
                # Below the trip threshold alone is insufficient for recovery.
                self.f.sensors.values[sensor] = self.thresholds[sensor] - EDGE_CLEAR_MARGIN
                self.f.advance(1200)
                self.assertFalse(self.control.ready)
                self.f.sensors.values[sensor] = 100
                self.f.advance(800)
                self.assertFalse(self.control.ready)
                self.f.advance(240)
                self.assertTrue(self.control.ready)

    def test_missing_samples_cannot_count_as_stable_floor(self):
        self.lift()
        self.f.sensors.values = [100] * 5
        self.f.advance(800)
        self.f.clock.now += 2000
        self.control.tick()
        self.assertFalse(self.control.ready)
        self.f.advance(800)
        self.assertFalse(self.control.ready)
        self.f.advance(220)
        self.assertTrue(self.control.ready)

    def test_sensor_error_or_malformed_sample_revokes_recovery(self):
        for bad in (None, [], [100] * 4, [100, 100, 100, 100, -1], [100] * 4 + [None]):
            with self.subTest(bad=bad):
                self.setUp()
                self.lift()
                self.f.sensors.fail = bad is None
                self.f.sensors.values = bad
                self.f.advance(EDGE_POLL_MS)
                self.assertEqual(self.control.fault, "SENSOR")
                self.f.sensors.fail = False
                self.put_down()
                self.assertFalse(self.control.ready)
                self.assertIsNone(self.control.edge_restore_thresholds)

    def test_other_faults_and_initial_uncalibrated_state_stay_locked(self):
        for reason in ("BUTTON_C", "SENSOR", "ENCODER", "TURN_FAULT", "CAL_FAILED"):
            with self.subTest(reason=reason):
                self.setUp()
                self.lift()
                self.control._latch(reason)
                self.put_down()
                self.assertFalse(self.control.ready)
                self.assertEqual(self.control.fault, reason)
                self.assertEqual(self.control.surface_thresholds, self.thresholds)
        self.f.setUp()
        self.f.advance(3000)
        self.assertFalse(self.f.control.ready)

    def test_held_button_blocks_restore_and_button_event_requires_calibration(self):
        self.lift()
        self.f.c.pressed = True
        self.put_down()
        self.assertFalse(self.control.ready)
        self.f.c.event = True
        self.control.tick()
        self.f.c.pressed = False
        self.put_down()
        self.assertEqual(self.control.fault, "BUTTON_C")
        self.assertFalse(self.control.ready)

    def test_stop_leaves_drive_cancelled_after_surface_restores(self):
        self.lift()
        self.f.command("5 STOP")
        self.put_down()
        self.assertTrue(self.control.ready)
        self.assertFalse(self.control.motion.armed)
        self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_manual_calibration_supersedes_old_recovery_even_if_interrupted(self):
        self.lift()
        self.f.sensors.values = [100] * 5
        self.f.advance(800)
        self.f.command("5 CALIBRATE")
        self.f.command("6 STOP")
        self.put_down()
        self.assertFalse(self.control.ready)
        self.assertIsNone(self.control.edge_restore_thresholds)

    def test_repeated_pickups_keep_the_same_calibration(self):
        for _ in range(3):
            self.lift()
            self.put_down()
            self.assertTrue(self.control.ready)
            self.assertEqual(self.control.thresholds, self.thresholds)
            self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_autonomous_session_remains_closed_after_surface_restores(self):
        f = motion_fixtures.MotionTests()
        f.setUp()
        f.drive()
        f.sensors.values = [1024] * 5
        f.step(20)
        self.assertFalse(f.job.armed)
        f.sensors.values = [100] * 5
        f.step(1100)
        self.assertTrue(f.control.ready)
        self.assertFalse(f.job.armed)
        self.assertIn("M2_SESSION_CLOSED", f.send(0, "ARM"))
        self.assertIn("M2_DISARMED", f.send(1, "DRIVE", "3000 3000 6000 50 10000"))
        self.assertEqual(f.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
