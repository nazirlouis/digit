"""Three-lap circle jobs through the production parser and safety loop."""
import unittest
import test_motion_jobs as motion_fixtures


class CircleTests(unittest.TestCase):
    setUp = motion_fixtures.MotionTests.setUp
    send = motion_fixtures.MotionTests.send
    step = motion_fixtures.MotionTests.step

    def start_circle(self, speed=4000):
        reply = self.send(1, "CIRCLE", "{} 3 6000 60000".format(speed))
        self.assertNotIn(" ERR ", reply)
        self.assertEqual(self.job.state, "CALIBRATING")

    def simulate(self, duration):
        for _ in range(duration // 5):
            left, right = self.motors.speeds
            self.gyro.last_reading_dps = (0, 0, (right-left) * .035)
            self.feedback.velocity = [left / 2, right / 2]
            self.step(5)

    def test_three_gyro_laps_at_dashboard_speed_then_stops(self):
        self.start_circle(4700)
        self.simulate(1200)
        self.assertEqual(self.control.target_left, 4700)
        self.assertEqual(self.control.target_right, 4700)
        self.assertEqual(self.control.track_speed_ratios, (1.0 / 3.0, 1.0))
        self.assertGreater(self.motors.speeds[0], 0)
        self.assertAlmostEqual(self.feedback.targets[1] / self.feedback.targets[0], 3)
        self.assertTrue(self.job.active)
        self.simulate(22000)
        self.assertEqual(self.job.reason, "CIRCLE_DONE")
        self.assertEqual(self.job.state, "DONE")
        self.assertGreaterEqual(self.job.heading-self.job.turn_origin, 1072)
        self.assertLess(self.job.heading-self.job.turn_origin, 1120)
        self.assertEqual(self.motors.speeds, (0, 0))
        # Duplicate frames cannot replay a completed dance.
        self.send(1, "CIRCLE", "4700 3 6000 60000")
        self.assertEqual(self.job.state, "DONE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_speed_limits_and_exactly_three_laps(self):
        for args in ("2999 3 6000 60000", "6001 3 6000 60000", "4000 4 6000 60000", "4000 3 6000 60001"):
            self.assertIn("ERR M2_ARGUMENT", self.send(1, "CIRCLE", args))
            self.assertEqual(self.motors.speeds, (0, 0))

    def test_fresh_no_return_starts_and_completes_without_faking_distance(self):
        self.tof.range_status = 2
        self.tof.distance_mm = 24  # Invalid raw distance must not be an obstacle.
        self.step(25)
        self.assertFalse(self.control._range_valid(self.clock.now))
        self.assertTrue(self.control._range_no_return(self.clock.now))
        self.assertEqual(self.control.distance_mm, -1)
        self.uart.send("99 STATUS"); self.control.tick()
        self.assertTrue(any("NR=1" in line and "RANGE=0" in line for line in self.uart.outgoing))
        self.start_circle()
        self.simulate(26000)
        self.assertEqual(self.job.reason, "CIRCLE_DONE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_intermittent_no_return_continues_then_real_obstacle_stops(self):
        self.start_circle(); self.simulate(1200)
        for _ in range(3):
            self.tof.range_status = 2
            self.simulate(300)
            self.assertTrue(self.job.active)
            self.tof.range_status = 0
            self.simulate(300)
            self.assertTrue(self.job.active)
        self.tof.distance_mm = 49
        self.simulate(25)
        self.assertEqual(self.job.reason, "OBSTACLE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_no_return_does_not_hide_stale_stream_bus_or_other_sensor_errors(self):
        for failure in ("stale", "bus", 1, 3, 4, 5, 12, 255):
            with self.subTest(failure=failure):
                self.setUp()
                self.tof.range_status = 2
                self.step(25); self.start_circle(); self.simulate(1200)
                if failure == "stale":
                    self.tof.data_ready = False
                elif failure == "bus":
                    def broken_read():
                        raise OSError("I2C disconnected")
                    self.tof.read_result = broken_read
                else:
                    self.tof.range_status = failure
                self.simulate(300)
                self.assertFalse(self.job.active)
                self.assertFalse(self.control._range_no_return(self.clock.now))
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_short_unreliable_sample_recovers_without_cancelling(self):
        self.start_circle(); self.simulate(1200)
        for status in (1, 4, 7, 12):
            self.tof.range_status = status
            self.simulate(100)
            self.assertTrue(self.job.active)
            self.tof.range_status = 0
            self.simulate(100)
            self.assertTrue(self.job.active)
        self.tof.distance_mm = 49
        self.simulate(25)
        self.assertEqual(self.job.reason, "OBSTACLE")

    def test_persistent_bad_sample_has_specific_latched_evidence(self):
        self.start_circle(); self.simulate(1200)
        self.tof.range_status = 1; self.tof.distance_mm = 234
        self.simulate(300)
        self.assertEqual(self.job.reason, "RANGE_STATUS_1")
        self.tof.range_status = 0; self.tof.distance_mm = 900
        self.simulate(30)
        self.assertTrue(self.control._range_valid(self.clock.now))
        self.job.snapshot()
        self.assertIn("RS=1 RAW=234", self.uart.outgoing[-1])

    def test_dance_lights_restore_on_stop_and_do_not_restart(self):
        saved = [color[:] for color in self.control.rgb_leds.colors]
        self.start_circle(); self.simulate(1200)
        self.assertNotEqual(self.control.rgb_leds.colors, saved)
        self.uart.send("99 STOP"); self.control.tick()
        self.assertEqual(self.control.rgb_leds.colors, saved)
        count = self.control.rgb_leds.shows
        self.step(100)
        self.assertEqual(self.control.rgb_leds.shows, count)

    def test_stop_cancels_circle(self):
        self.start_circle(); self.simulate(1200)
        self.uart.send("99 STOP"); self.control.tick()
        self.assertFalse(self.job.active)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.control.track_speed_ratios)

    def test_manual_takeover_restores_equal_track_targets(self):
        self.start_circle(); self.simulate(1200)
        self.uart.send("99 DRIVE 4000 4000 6000"); self.control.tick()
        self.assertIsNone(self.control.track_speed_ratios)
        self.assertFalse(self.job.active)
        self.step(25)
        self.assertEqual(self.feedback.targets[0], self.feedback.targets[1])

    def test_circle_start_and_running_boundary_is_strictly_below_five_cm(self):
        self.tof.distance_mm = 49
        self.step(25)
        self.assertIn("ERR M2_OBSTACLE", self.send(1, "CIRCLE", "4000 3 6000 60000"))
        self.tof.distance_mm = 50
        self.step(25); self.start_circle(); self.simulate(1200)
        self.assertTrue(self.job.active)
        self.assertGreater(self.motors.speeds[1], 0)
        self.tof.distance_mm = 49
        self.simulate(25)
        self.assertEqual(self.job.reason, "OBSTACLE")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_heartbeat_failure_cancels_circle(self):
        self.start_circle(); self.simulate(1200)
        self.step(800, heartbeat=False)
        self.assertFalse(self.job.active)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_obstacle_cancels_circle(self):
        self.start_circle(); self.simulate(1200)
        self.tof.distance_mm = 49
        self.simulate(250)
        self.assertFalse(self.job.active)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_wrong_direction_and_stall_cancel(self):
        self.start_circle(); self.step(450)
        self.gyro.last_reading_dps = (0, 0, -100)
        self.step(100)
        self.assertEqual(self.job.reason, "CIRCLE_DIRECTION")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_gyro_stall_cancels_without_waiting_full_deadline(self):
        self.start_circle(); self.step(2000)
        self.assertEqual(self.job.reason, "CIRCLE_STALLED")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_sensor_failure_cancels_circle(self):
        self.start_circle(); self.simulate(1200)
        self.gyro.fail = True
        self.step(10)
        self.assertFalse(self.job.active)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_edge_button_encoder_stale_range_and_control_gap_stop(self):
        for failure in ("edge", "button", "encoder", "range", "gap"):
            with self.subTest(failure=failure):
                self.setUp(); self.start_circle(); self.simulate(1200)
                if failure == "edge": self.sensors.values[0] = 950
                elif failure == "button": self.c.pressed = self.c.event = True
                elif failure == "encoder": self.feedback.fail = True
                elif failure == "range": self.tof.data_ready = False
                else: self.clock.now += 200
                self.step(300 if failure == "range" else 30)
                self.assertFalse(self.job.active)
                self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
