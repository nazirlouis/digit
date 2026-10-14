"""Exercise persistent jobs through the real UART parser/control/safety loop."""
import unittest
from test_uart_control import Clock, UART, Motors, Sensors, Battery, Face, RGBLEDs, Button
from motion_jobs import crc16, frame
from test_voice_turn import Gyro
from test_automatic_drive import RangeSensor
from uart_control import UARTController


class Feedback:
    def __init__(self, clock):
        self.at = clock.ticks_ms()
        self.velocity = [0, 0]
        self.targets = [0, 0]
        self.fail = False
    def reset(self):
        self.targets = [0, 0]
    def update(self, now, targets, outputs, braking=False, rate_ratios=None):
        self.at = now
        if self.fail:
            raise RuntimeError("encoder unplugged")
        self.targets = tuple(targets[i] * rate_ratios[i] for i in range(2)) if rate_ratios else targets
        return (0, 0) if braking else self.targets


class MotionTests(unittest.TestCase):
    def setUp(self):
        self.clock, self.uart, self.motors = Clock(), UART(), Motors()
        self.sensors, self.gyro, self.tof = Sensors(), Gyro(), RangeSensor()
        self.a, self.b, self.c = Button(), Button(), Button()
        self.feedback = Feedback(self.clock)
        self.control = UARTController(self.uart, self.motors, self.sensors, Battery(), Face(),
                                      self.a, self.b, self.c, self.clock, RGBLEDs(), self.gyro,
                                      tof=self.tof, speed_control=self.feedback)
        self.control.start_calibration()
        self.step(600, heartbeat=False)
        self.job = self.control.motion
        self.boot = self.job.boot
        self.session = 1
        self.send(0, "ARM")
        self.assertTrue(self.job.armed)

    def send(self, seq, verb, args="", session=None, boot=None, corrupt=False):
        body = "M2 {} {} {} {}".format(boot or self.boot, self.session if session is None else session, seq, verb)
        if args:
            body += " " + args
        text = frame("10", body)
        if corrupt:
            text = text[:-1] + ("0" if text[-1] != "0" else "1")
        self.uart.send(text)
        self.control.tick()
        return self.uart.outgoing[-1]

    def step(self, duration, heartbeat=True):
        for _ in range(duration // 5):
            self.clock.now += 5
            if heartbeat and self.clock.now % 100 == 0:
                self.uart.send(frame("11", "M2 {} {} 0 HB {}".format(self.boot, self.session, self.clock.now)))
            self.control.tick()

    def drive(self):
        self.send(1, "DRIVE", "3000 3000 6000 50 10000")
        self.step(950)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertGreater(self.motors.speeds[0], 0)

    def test_drive_obstacle_boundary_for_both_arrival_targets(self):
        for stop_mm in (50, 100):
            with self.subTest(stop_mm=stop_mm):
                self.setUp()
                self.tof.distance_mm = 49; self.step(25)
                args = "3000 3000 6000 {} 10000".format(stop_mm)
                self.assertIn("ERR M2_OBSTACLE", self.send(1, "DRIVE", args))
                self.tof.distance_mm = 50; self.step(25)
                self.assertNotIn(" ERR ", self.send(1, "DRIVE", args))
                self.step(25)
                self.assertNotEqual(self.job.reason, "OBSTACLE")
                self.tof.distance_mm = 49; self.step(25)
                self.assertEqual(self.job.reason, "OBSTACLE")
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_crc_reference_and_corrupt_boot_frames_cannot_move(self):
        self.assertEqual(crc16("123456789"), 0x29b1)
        self.assertIn("ERR M2_CRC", self.send(1, "DRIVE", "3000 3000 6000 50 10000", corrupt=True))
        self.assertIn("ERR M2_BOOT", self.send(1, "DRIVE", "3000 3000 6000 50 10000", boot="deadbeef"))
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.job.seq, 0)

    def test_job_persists_with_heartbeat_and_retry_does_not_restart(self):
        self.drive()
        started = self.job.started
        self.send(1, "DRIVE", "3000 3000 6000 50 10000")
        self.assertEqual(self.job.started, started)
        self.assertIn("ERR M2_SEQ", self.send(1, "DRIVE", "3100 3100 6000 50 10000"))
        self.step(1000)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertIsNone(self.control.last_drive)

    def test_heartbeat_loss_locally_stops_and_old_session_cannot_resume(self):
        self.drive()
        self.step(760, heartbeat=False)
        self.assertEqual(self.job.reason, "HEARTBEAT_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("ERR M2_SESSION_CLOSED", self.send(0, "ARM"))
        self.assertIn("ERR M2_DISARMED", self.send(1, "DRIVE", "3000 3000 6000 50 10000"))
        self.session = 2
        self.send(0, "ARM")
        self.send(0, "CANCEL")
        self.assertIn("ERR M2_SESSION_CLOSED", self.send(0, "ARM", session=1))

    def test_stop_cancels_even_without_valid_crc_and_no_delayed_replay(self):
        self.drive()
        self.uart.send("12 STOP")
        self.control.tick()
        self.assertEqual(self.job.state, "DISARMED")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.send(0, "HB", "999999")
        self.send(1, "DRIVE", "3000 3000 6000 50 10000")
        self.step(1000)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_replayed_heartbeat_does_not_extend_supervisor_lease(self):
        self.drive()
        counter = self.job.heartbeat_counter
        last = self.job.heartbeat
        for _ in range(8):
            self.step(100, heartbeat=False)
            self.send(0, "HB", str(counter))
        self.assertEqual(self.job.heartbeat, last)
        self.assertEqual(self.job.reason, "HEARTBEAT_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_arrival_waits_for_encoders_not_just_zero_output(self):
        self.drive()
        self.feedback.velocity = [400, 400]
        self.tof.distance_mm = 50
        self.step(20)
        response = self.send(1, "UPDATE", "5 3000 3200")
        self.assertIn(" BRAKING WALL_BRAKING ", response)
        self.assertEqual((self.control.target_left, self.control.target_right), (0, 0))
        self.assertEqual(self.job.state, "BRAKING")
        self.step(500)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.job.state, "BRAKING")
        self.assertNotEqual(self.job.reason, "WALL_STOPPED")
        self.feedback.velocity = [0, 0]
        self.step(160)
        self.assertEqual((self.job.state, self.job.reason), ("DONE", "WALL_STOPPED"))
        self.send(1, "UPDATE", "1 3100 3000")
        self.assertEqual(self.job.state, "DONE")
        self.step(200)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_initial_wall_does_not_start_tracks(self):
        self.tof.distance_mm = 50
        self.step(20)
        self.send(1, "DRIVE", "3000 3000 6000 50 10000")
        for _ in range(150):
            self.step(5)
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.job.reason, "WALL_STOPPED")

    def test_measured_closing_rate_brakes_before_wall(self):
        self.drive()
        for distance in (300, 280, 260, 240, 220, 200):
            self.tof.distance_mm = distance
            self.step(20)
            if self.job.state == "BRAKING":
                break
        self.assertGreater(self.tof.distance_mm, 50)
        self.assertEqual(self.job.state, "BRAKING")

    def test_single_range_jump_does_not_invent_high_closing_speed(self):
        self.drive()
        self.tof.distance_mm = 250
        self.step(20)
        self.assertEqual(self.job.state, "RUNNING")
        self.step(100)
        self.assertEqual(self.job.closing_mm_s, 0)
        self.assertEqual(self.job.state, "RUNNING")

    def test_stopping_short_continues_same_job_without_false_wall_event(self):
        self.drive()
        started = self.job.started
        for distance in (300, 280, 260, 240, 220):
            self.tof.distance_mm = distance
            self.step(20)
        self.assertEqual(self.job.state, "BRAKING")
        self.step(500)
        self.assertEqual(self.job.state, "RUNNING")
        self.assertEqual(self.job.reason, "APPROACHING")
        self.assertEqual(self.job.started, started)
        self.assertNotIn("WALL_STOPPED", " ".join(self.uart.outgoing))

    def test_gyro_io_error_has_machine_readable_fault_and_stops(self):
        self.drive()
        self.gyro.fail = True
        self.step(5)
        self.assertEqual(self.job.reason, "GYRO_IO")
        self.assertIn(" RECOVERING GYRO_IO ", self.uart.outgoing[-1])
        self.step(2050)
        self.assertEqual(self.job.reason, "RECOVERY_FAILED")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_sensor_edge_button_encoder_and_loop_gap_fail_closed(self):
        for failure in ("invalid", "stale", "edge", "button", "encoder", "gap", "near"):
            with self.subTest(failure=failure):
                self.setUp(); self.drive()
                if failure == "invalid":
                    self.tof.range_status = 4
                elif failure == "stale":
                    self.tof.data_ready = False
                elif failure == "edge":
                    self.sensors.values[0] = 950
                elif failure == "button":
                    self.c.pressed = self.c.event = True
                elif failure == "encoder":
                    self.feedback.fail = True
                elif failure == "gap":
                    self.clock.now += 151
                else:
                    self.tof.distance_mm = 30
                self.step(270 if failure in ("stale", "invalid") else 30)
                if failure == "edge" and hasattr(self.feedback, "encoders") and self.job.distance_scale is not None:
                    self.assertTrue(self.job.armed)
                    self.assertEqual(self.job.reason, "EDGE_BACK")
                    self.assertLessEqual(self.motors.speeds[0], -3000)
                    continue
                self.assertEqual(self.motors.speeds, (0, 0))
                if failure in ("edge", "button", "encoder"):
                    self.assertFalse(self.job.armed)
                    self.assertEqual(self.job.state, "FAULT")
                else:
                    self.assertTrue(self.job.armed)
                    self.assertEqual(self.job.state, "RECOVERING")

    def test_heading_correction_and_update_preserve_deadline(self):
        self.drive()
        started = self.job.started
        self.gyro.last_reading_dps = (0, 0, 20)
        self.step(100)
        self.assertGreater(self.control.target_left, self.control.target_right)
        self.send(1, "UPDATE", "1 3000 3100")
        self.assertEqual(self.job.started, started)
        self.assertEqual((self.job.left, self.job.right), (3000, 3100))
        self.assertIn("ERR M2_SEQ", self.send(1, "UPDATE", "1 3100 3000"))
        self.send(1, "UPDATE", "2 3000 3000")
        self.assertAlmostEqual(self.job.target_heading, self.job.heading)

    def test_deadline_cannot_be_extended_by_heartbeat_or_updates(self):
        self.send(1, "DRIVE", "3000 3000 6000 50 1200")
        self.step(1000)
        self.send(1, "UPDATE", "1 3000 3100")
        self.step(205)
        self.assertEqual(self.job.reason, "JOB_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_measure_waits_for_settle_then_three_new_samples(self):
        self.feedback.velocity = [100, 100]
        self.assertIn("ERR M2_NOT_STOPPED", self.send(1, "MEASURE", "2000"))
        self.feedback.velocity = [0, 0]
        self.send(1, "MEASURE", "2000")
        self.step(155)
        self.assertEqual(self.job.measure_samples, [])
        self.tof.distance_mm = 300; self.step(20)
        self.tof.distance_mm = 800; self.step(20)
        self.tof.distance_mm = 310; self.step(20)
        self.assertEqual((self.job.state, self.job.reason, self.job.measured), ("DONE", "MEASURED", 310))
        before = len(self.uart.outgoing)
        self.step(300)
        self.assertGreater(len(self.uart.outgoing), before)
        self.send(1, "ACK")
        before = len(self.uart.outgoing)
        self.step(300)
        self.assertEqual(len(self.uart.outgoing), before)
        self.uart.send("20 STATUS"); self.control.tick()
        self.assertIn("MEASURED 310", self.uart.outgoing[-1])
        self.assertIn("ERR M2_SEQ", self.send(1, "MEASURE", "3000"))

    def test_normal_brake_closes_session_but_keeps_ramp_and_local_guard(self):
        self.drive()
        before = self.motors.speeds
        self.uart.send("12 BRAKE"); self.control.tick()
        self.assertEqual(self.motors.speeds, before)
        self.assertFalse(self.job.armed)
        self.assertTrue(self.control.braking)
        self.assertTrue(self.control.safe_motion)
        self.step(50)
        self.assertGreater(self.motors.speeds[0], 0)
        self.assertLess(self.motors.speeds[0], before[0])
        self.tof.distance_mm = 30
        self.step(20)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT OBSTACLE", self.uart.outgoing)

    def test_missing_new_range_measurement_times_out(self):
        self.tof.data_ready = False
        self.send(1, "MEASURE", "400")
        self.step(405)
        self.assertEqual(self.job.reason, "JOB_TIMEOUT")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_turn_runs_locally_and_cumulative_heading_is_retained(self):
        for seq, target in ((1, 90), (2, -90)):
            self.send(seq, "TURN", "{} 6000 12000".format(target))
            for _ in range(2400):
                left, right = self.motors.speeds
                self.gyro.last_reading_dps = (0, 0, (right-left)/30)
                self.step(5)
                if not self.job.active:
                    break
            self.assertEqual((self.job.state, self.job.reason), ("DONE", "TURN_DONE"))
            self.assertEqual(self.motors.speeds, (0, 0))
            self.assertAlmostEqual(self.job.heading, 90 if seq == 1 else 0, delta=10)

    def test_missing_feedback_prevents_arm(self):
        self.send(0, "CANCEL")
        self.control.speed_control = None
        self.assertIn("ERR M2_FEEDBACK_REQUIRED", self.send(0, "ARM", session=2))
        self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
