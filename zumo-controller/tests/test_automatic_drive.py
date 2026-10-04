"""Exercise real UART safety logic with a controllable distance sensor."""
import unittest
import test_uart_control as fixtures


class RangeSensor:
    data_ready = True
    range_status = 0
    distance_mm = 900
    def clear_interrupt(self):
        pass


class AutomaticDriveTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.UARTControlTests()
        self.fixture.setUp()
        self.fixture.control.tof = self.tof = RangeSensor()
        self.fixture.calibrate()

    def send(self, command):
        return self.fixture.command(command)

    def advance(self, ms):
        self.fixture.clock.now += ms
        self.fixture.control.tick()

    def test_stop_cancels_safe_drive_and_does_not_restart(self):
        self.assertEqual(self.send("2 SAFE 1200 1200"), "2 OK SAFE")
        self.assertEqual(self.send("3 STOP"), "3 OK STOPPED")
        for _ in range(40):
            self.advance(50)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertFalse(self.fixture.control.safe_motion)
        self.assertEqual(self.fixture.face.actions[-1], "neutral")

    def test_near_obstacle_rejects_forward_and_stops_active_motion(self):
        self.send("2 SAFE 1200 1200")
        self.tof.distance_mm = 140
        self.advance(20)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertIn("0 EVENT OBSTACLE", self.fixture.uart.outgoing)
        self.assertEqual(self.send("3 SAFE 1200 1200"), "3 ERR OBSTACLE")
        self.assertEqual(self.send("4 SAFE -1000 1000"), "4 OK SAFE")

    def test_missing_invalid_or_stale_distance_never_means_clear(self):
        self.fixture.control.tof = None
        self.assertEqual(self.send("2 SAFE 1200 1200"), "2 ERR RANGE_UNAVAILABLE")
        self.fixture.control.tof = self.tof
        self.tof.range_status = 4
        self.advance(20)
        self.assertEqual(self.send("3 SAFE 1200 1200"), "3 ERR RANGE_UNAVAILABLE")
        self.tof.range_status = 0
        self.advance(20)
        self.send("4 SAFE 1200 1200")
        self.tof.data_ready = False
        self.advance(260)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertEqual(self.fixture.control.fault, "SENSOR")
        self.assertFalse(self.fixture.control.ready)

    def test_edge_is_latched_without_reverse_or_resume(self):
        self.send("2 SAFE 1200 1200")
        self.fixture.sensors.values[0] = 950
        self.advance(20)
        self.assertEqual(self.fixture.control.fault, "EDGE")
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.fixture.sensors.values[0] = 100
        self.assertEqual(self.send("3 SAFE 1200 1200"), "3 ERR NOT_READY")
        self.advance(1000)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))

    def test_automatic_drive_requires_continued_transport(self):
        self.send("2 SAFE 1200 1200")
        self.advance(301)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.fixture.uart.outgoing)

    def test_manual_drive_keeps_existing_sensor_contract(self):
        self.fixture.control.tof = None
        self.assertEqual(self.send("2 DRIVE 1000 1000"), "2 OK DRIVE")
        self.assertFalse(self.fixture.control.safe_motion)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.fixture.advance(100)
        self.assertEqual(self.fixture.motors.speeds, (300, 300))

    def test_status_advertises_updated_controller_and_distance(self):
        self.assertIn("DIST=900 RANGE=1 AUTO=1", self.send("2 STATUS"))

    def test_front_distance_safety_remains_active_while_braking(self):
        for failure in ("obstacle", "stale"):
            with self.subTest(failure=failure):
                self.setUp()
                self.send("2 SAFE 1200 1200")
                self.fixture.advance(410, "2 SAFE 1200 1200")
                self.assertEqual(self.fixture.motors.speeds, (1200, 1200))
                self.send("3 BRAKE")
                self.assertTrue(self.fixture.control.safe_motion)
                if failure == "obstacle":
                    self.tof.distance_mm = 140
                    self.advance(20)
                else:
                    self.tof.data_ready = False
                    self.advance(260)
                self.assertEqual(self.fixture.motors.speeds, (0, 0))
                self.assertFalse(self.fixture.control.braking)


if __name__ == "__main__":
    unittest.main()
