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
    def test_close_approach_passes_15cm_and_brakes_locally_at_10cm(self):
        command = "2 SAFE 2400 2400 6000 100"
        self.assertEqual(self.send(command), "2 OK SAFE")
        self.fixture.advance(520, command)
        self.assertEqual(self.fixture.motors.speeds, (3000, 3000))
        self.tof.distance_mm = 140
        self.advance(20)
        self.assertEqual(self.fixture.motors.speeds, (3000, 3000))
        self.tof.distance_mm = 100
        self.advance(20)
        self.assertTrue(self.fixture.control.braking)
        self.assertIn("0 EVENT ARRIVED", self.fixture.uart.outgoing)
        self.tof.distance_mm = 110
        self.assertEqual(self.send(command), "2 OK ARRIVED")
        self.fixture.advance(260)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertTrue(self.fixture.control.ready)
        self.assertNotIn("0 EVENT OBSTACLE", self.fixture.uart.outgoing)
        self.send("3 STOP")
        self.assertFalse(self.fixture.control.approach_arrived)
        self.assertEqual(self.send("4 SAFE 2400 2400 6000"), "4 ERR OBSTACLE")

    def test_close_approach_keeps_emergency_range_edge_and_stale_sensor_stops(self):
        for failure in ("near", "stale", "edge"):
            with self.subTest(failure=failure):
                self.setUp()
                self.send("2 SAFE 2400 2400 6000 100")
                self.fixture.advance(520, "2 SAFE 2400 2400 6000 100")
                if failure == "near":
                    self.tof.distance_mm = 80
                    self.advance(20)
                    self.assertIn("0 EVENT OBSTACLE", self.fixture.uart.outgoing)
                elif failure == "stale":
                    self.tof.data_ready = False
                    self.advance(260)
                    self.assertEqual(self.fixture.control.fault, "SENSOR")
                else:
                    self.fixture.sensors.values[0] = 950
                    self.advance(20)
                    self.assertEqual(self.fixture.control.fault, "EDGE")
                self.assertEqual(self.fixture.motors.speeds, (0, 0))

    def test_close_approach_arguments_and_manual_takeover(self):
        for command in ("SAFE 2400 2400 6000 20", "SAFE -2400 2400 6000 100", "DRIVE 2400 2400 6000 100"):
            self.assertIn(" ERR ", self.send("2 " + command))
        self.send("2 SAFE 2400 2400 6000 100")
        self.fixture.advance(100)
        self.send("3 DRIVE 2400 2400 6000")
        self.assertEqual(self.fixture.control.approach_stop_mm, 0)
        self.assertFalse(self.fixture.control.safe_motion)
        self.assertIn("CLOSE=2", self.send("4 STATUS"))

    def test_chase_passes_10cm_and_brakes_locally_at_5cm(self):
        command = "2 SAFE 2400 2400 6000 50"
        self.assertEqual(self.send(command), "2 OK SAFE")
        self.fixture.advance(520, command)
        for distance in (140, 100, 80, 60, 51):
            self.tof.distance_mm = distance
            self.advance(20)
            self.assertEqual(self.fixture.motors.speeds, (3000, 3000))
        self.tof.distance_mm = 50
        self.advance(20)
        self.assertTrue(self.fixture.control.braking)
        self.assertIn("0 EVENT ARRIVED", self.fixture.uart.outgoing)
        self.tof.distance_mm = 60
        self.assertEqual(self.send(command), "2 OK ARRIVED")
        self.fixture.advance(260)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        self.assertTrue(self.fixture.control.ready)
        self.assertNotIn("0 EVENT OBSTACLE", self.fixture.uart.outgoing)
        self.send("3 STOP")
        self.tof.distance_mm = 100
        self.advance(20)
        self.assertEqual(self.send(command), "2 OK SAFE")
        self.fixture.advance(520, command)
        self.assertEqual(self.fixture.motors.speeds, (3000, 3000))

    def test_chase_keeps_hard_stop_while_braking_and_sensor_safeties(self):
        for failure in ("near", "stale", "edge", "timeout"):
            with self.subTest(failure=failure):
                self.setUp()
                command = "2 SAFE 2400 2400 6000 50"
                self.send(command)
                self.fixture.advance(520, command)
                if failure == "near":
                    self.tof.distance_mm = 50
                    self.advance(20)
                    self.assertTrue(self.fixture.control.braking)
                    self.tof.distance_mm = 30
                    self.advance(20)
                    self.assertIn("0 EVENT OBSTACLE", self.fixture.uart.outgoing)
                    self.assertEqual(self.send(command), "2 ERR OBSTACLE")
                elif failure == "stale":
                    self.tof.data_ready = False
                    self.advance(260)
                    self.assertEqual(self.fixture.control.fault, "SENSOR")
                elif failure == "edge":
                    self.fixture.sensors.values[0] = 950
                    self.advance(20)
                    self.assertEqual(self.fixture.control.fault, "EDGE")
                else:
                    self.advance(301)
                    self.assertIn("0 EVENT TIMEOUT", self.fixture.uart.outgoing)
                self.assertEqual(self.fixture.motors.speeds, (0, 0))

    def test_extended_safe_ramps_above_dead_zone_with_selected_acceleration(self):
        for acceleration in (500, 3000, 6000):
            with self.subTest(acceleration=acceleration):
                self.setUp()
                command = "2 SAFE 2800 2800 {}".format(acceleration)
                self.assertEqual(self.send(command), "2 OK SAFE")
                self.fixture.advance(100, command)
                self.assertEqual(self.fixture.motors.speeds, (acceleration // 10,) * 2)
                self.fixture.advance(6000, command)
                self.assertEqual(self.fixture.motors.speeds, (3000, 3000))
                self.send("3 BRAKE")
                self.fixture.advance(250)
                self.assertEqual(self.fixture.motors.speeds, (0, 0))

    def test_extended_safe_limits_and_lease_remain_enforced(self):
        self.assertEqual(self.send("2 SAFE -2800 2800 6000"), "2 ERR AUTO_SPEED_RANGE")
        self.assertEqual(self.send("3 SAFE 6000 6000 6000"), "3 ERR AUTO_SPEED_RANGE")
        self.assertEqual(self.send("4 SAFE 2800 2800 1"), "4 ERR ACCEL_RANGE")
        self.assertEqual(self.send("5 SAFE 2800 2800 6000"), "5 OK SAFE")
        self.fixture.advance(280)
        self.assertGreater(self.fixture.motors.speeds[0], 0)
        self.fixture.advance(25)
        self.assertEqual(self.fixture.motors.speeds, (0, 0))
        status = self.send("6 STATUS")
        self.assertIn("SCENE=1 SAFE2=1", status)
        self.assertLess(len(status), 320)

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
                self.assertEqual(self.fixture.motors.speeds, (1230, 1230))
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
