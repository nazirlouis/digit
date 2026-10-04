"""Normal braking, immediate safety overrides, and marginal edge filtering."""
import unittest
import test_uart_control as fixtures
from uart_control import DECELERATION, BRAKE_TIMEOUT_MS


class BrakingAndEdgeTests(unittest.TestCase):
    def setUp(self):
        self.f = fixtures.UARTControlTests()
        self.f.setUp()
        self.f.calibrate()

    def moving(self, speed=4000, acceleration=6000):
        command = "2 DRIVE {} {} {}".format(speed, speed, acceleration)
        self.f.command(command)
        self.f.advance(abs(speed) * 1000 // acceleration + 10, command)
        self.assertEqual(self.f.motors.speeds, (speed, speed))

    def test_braking_uses_fixed_rate_for_both_directions_and_slider_extremes(self):
        for speed in (4000, -4000):
            for acceleration in (500, 6000):
                with self.subTest(speed=speed, acceleration=acceleration):
                    self.setUp()
                    self.moving(speed, acceleration)
                    self.assertEqual(self.f.command("3 BRAKE"), "3 OK BRAKING")
                    self.assertEqual(self.f.motors.speeds, (speed, speed))
                    self.assertIsNone(self.f.control.last_drive)
                    self.assertEqual((self.f.control.target_left, self.f.control.target_right), (0, 0))
                    self.assertEqual(self.f.command("4 FACE HAPPY"), "4 ERR MOVING")
                    self.f.advance(100)
                    expected = (abs(speed) - DECELERATION // 10) * (1 if speed > 0 else -1)
                    self.assertEqual(self.f.motors.speeds, (expected, expected))
                    self.f.advance(235)
                    self.assertEqual(self.f.motors.speeds, (0, 0))
                    self.assertFalse(self.f.control.braking)
                    self.f.advance(1000)
                    self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_duplicate_brake_does_not_reset_progress_or_deadline(self):
        self.moving(6000)
        self.f.command("3 BRAKE")
        started = self.f.control.brake_started
        self.f.clock.now += 100
        self.f.command("4 BRAKE")
        self.assertEqual(self.f.control.brake_started, started)
        self.assertEqual(self.f.motors.speeds, (5760, 5760))
        self.f.clock.now = started + BRAKE_TIMEOUT_MS
        self.f.control.tick()
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.f.uart.outgoing)

    def test_brake_finishes_after_drive_watchdog_without_requiring_keepalive(self):
        self.moving(6000)
        self.f.command("3 BRAKE")
        self.f.advance(400)
        self.assertEqual(self.f.motors.speeds, (1200, 1200))
        self.assertNotIn("0 EVENT TIMEOUT", self.f.uart.outgoing)
        self.f.advance(100)
        self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_hard_stop_edge_button_and_sensor_error_override_braking(self):
        for fault in ("STOP", "EDGE", "BUTTON_C", "SENSOR"):
            with self.subTest(fault=fault):
                self.setUp()
                self.moving()
                self.f.command("3 BRAKE")
                self.f.advance(50)
                self.assertEqual(self.f.motors.speeds, (3400, 3400))
                if fault == "STOP":
                    self.f.command("4 STOP")
                elif fault == "EDGE":
                    self.f.sensors.values[0] = 980
                elif fault == "BUTTON_C":
                    self.f.c.event = True
                else:
                    self.f.sensors.fail = True
                self.f.advance(20)
                self.assertEqual(self.f.motors.speeds, (0, 0))
                self.assertFalse(self.f.control.braking)
                self.f.advance(500)
                self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_marginal_lift_below_relaxed_threshold_and_single_poll_do_not_latch(self):
        self.moving()
        self.f.sensors.values[0] = 600  # Above the previous 562 threshold.
        self.f.advance(100, "2 DRIVE 4000 4000 6000")
        self.assertTrue(self.f.control.ready)
        self.f.sensors.values[0] = 700
        self.f.advance(20)
        self.assertTrue(self.f.control.ready)
        self.f.sensors.values[0] = 100
        self.f.advance(20)
        self.assertTrue(self.f.control.ready)
        self.assertEqual(self.f.motors.speeds, (4000, 4000))

    def test_sustained_marginal_edge_hard_stops_and_latches(self):
        self.moving()
        self.f.sensors.values[0] = 700
        self.f.advance(20)
        self.assertTrue(self.f.control.ready)
        self.f.advance(20)
        self.assertEqual(self.f.control.fault, "EDGE")
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.f.sensors.values[0] = 100
        self.assertEqual(self.f.command("3 DRIVE 1000 1000"), "3 ERR NOT_READY")

    def test_edge_confirmation_does_not_accumulate_across_sensors_or_same_tick(self):
        self.moving()
        self.f.sensors.values[0] = 700
        self.f.advance(20)
        for _ in range(4):
            self.f.command("2 DRIVE 4000 4000 6000")
        self.assertTrue(self.f.control.ready)
        self.f.sensors.values = [100, 700, 100, 100, 100]
        self.f.advance(20)
        self.assertTrue(self.f.control.ready)
        self.f.sensors.values = [100] * 5
        self.f.advance(20)
        self.assertTrue(self.f.control.ready)

    def test_brake_with_no_calibration_cannot_arm_or_move(self):
        self.f.control.ready = False
        self.assertEqual(self.f.command("3 BRAKE"), "3 OK STOPPED")
        self.assertFalse(self.f.control.ready)
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.assertIn("BRAKE=1", self.f.command("4 STATUS"))


if __name__ == "__main__":
    unittest.main()
