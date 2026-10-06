"""Exercise real UART/gyro control with a simulated turn and hardware faults."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from uart_control import UARTController
from voice_turn import (MIN_TURN_SPEED, MAX_TURN_SPEED, WIGGLE_SPEED,
                        WIGGLE_ACCELERATION, WIGGLE_DECELERATION)
from test_uart_control import Clock, UART, Motors, Sensors, Battery, Face, RGBLEDs, Button


class Gyro:
    def __init__(self):
        self.last_reading_dps = (0, 0, 0)
        self.ready = True
        self.fail = False

    def data_ready(self):
        return self.ready

    def read(self):
        if self.fail:
            raise OSError("gyro bus failed")


class VoiceTurnTests(unittest.TestCase):
    def setUp(self):
        self.clock, self.uart, self.motors = Clock(), UART(), Motors()
        self.sensors, self.gyro = Sensors(), Gyro()
        self.a, self.b, self.c = Button(), Button(), Button()
        self.controller = UARTController(
            self.uart, self.motors, self.sensors, Battery(), Face(),
            self.a, self.b, self.c, self.clock, RGBLEDs(), self.gyro)
        self.send("1 CALIBRATE")
        for _ in range(60):
            self.step(10)
        self.assertTrue(self.controller.ready)

    def send(self, line):
        self.uart.send(line)
        self.controller.tick()
        return self.uart.outgoing[-1]

    def step(self, ms=5, keep=True):
        self.clock.now += ms
        if keep and self.controller.look_id and self.clock.now % 100 == 0:
            self.uart.send("50 KEEP " + self.controller.look_id)
        self.controller.tick()

    def start_motion(self, angle=90):
        self.assertEqual(self.send("2 LOOK " + str(angle)), "2 OK LOOKING")
        for _ in range(82):
            self.step()
        self.assertNotEqual(self.motors.speeds, (0, 0))

    def test_real_control_reaches_left_right_and_rear_once(self):
        for target in (90, -90, 180, -180, 0):
            with self.subTest(target=target):
                self.gyro.last_reading_dps = (0, 0, 1.2)  # Stationary gyro bias.
                self.send("2 LOOK " + str(target))
                measured = 0.0
                for _ in range(1200):
                    left, right = self.motors.speeds
                    rate = (right - left) / 30
                    measured += rate * 0.005
                    self.gyro.last_reading_dps = (0, 0, rate + 1.2)
                    self.step()
                    if self.controller.look_id is None:
                        break
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertIn("2 OK LOOK_DONE", self.uart.outgoing)
                self.assertAlmostEqual(measured, target, delta=9)
                self.assertTrue(self.controller.ready)
                self.assertEqual(self.send("50 KEEP 2"), "50 ERR NO_LOOK")
                for _ in range(100):
                    self.step()
                self.assertEqual(self.motors.speeds, (0, 0))
                self.uart.outgoing.clear()

    def test_missing_heartbeat_stops_even_while_calibrating_gyro(self):
        self.send("2 LOOK 90")
        self.step(300, keep=False)
        self.assertIsNone(self.controller.look_id)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.send("3 KEEP 2"), "3 ERR NO_LOOK")

    def test_wrong_token_cannot_extend_lease(self):
        self.start_motion()
        self.assertEqual(self.send("3 KEEP 999"), "3 ERR NO_LOOK")
        self.step(300, keep=False)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.controller.look_id)

    def test_stop_and_manual_drive_supersede_turn(self):
        self.start_motion()
        self.assertEqual(self.send("3 STOP"), "3 OK STOPPED")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.controller.look_id)
        self.start_motion()
        self.assertEqual(self.send("4 DRIVE 800 800"), "4 OK DRIVE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.controller.look_id)
        self.step(5, keep=False)
        self.assertEqual(self.motors.speeds, (15, 15))
        self.step(300, keep=False)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_edge_button_and_sensor_failure_latch_off(self):
        for fault in ("EDGE", "BUTTON_C", "SENSOR"):
            with self.subTest(fault=fault):
                self.setUp()
                self.start_motion()
                if fault == "EDGE":
                    self.sensors.values[0] = 980
                elif fault == "BUTTON_C":
                    self.c.event = True
                else:
                    self.sensors.fail = True
                self.step(20)
                self.assertEqual(self.controller.fault, fault)
                self.assertFalse(self.controller.ready)
                self.assertIsNone(self.controller.look_id)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertEqual(self.send("3 LOOK 90"), "3 ERR NOT_READY")

    def test_gyro_stale_nan_io_stall_and_wrong_direction_latch_off(self):
        for mode, error in (("stale", "LOOK_GYRO"), ("nan", "LOOK_GYRO"),
                            ("io", "LOOK_GYRO"), ("stalled", "LOOK_STALLED"),
                            ("reversed", "LOOK_DIRECTION")):
            with self.subTest(mode=mode):
                self.setUp()
                self.start_motion()
                if mode == "stale": self.gyro.ready = False
                if mode == "nan": self.gyro.last_reading_dps = (0, 0, float("nan"))
                if mode == "io": self.gyro.fail = True
                if mode == "reversed": self.gyro.last_reading_dps = (0, 0, -90)
                for _ in range(260): self.step()
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertEqual(self.controller.fault, "TURN_FAULT")
                self.assertFalse(self.controller.ready)
                self.assertIn("2 ERR " + error, self.uart.outgoing)

    def test_gates_arguments_duplicate_and_held_button(self):
        for arg in ("nan", "90.0", "1 x", ""):
            self.assertEqual(self.send("2 LOOK " + arg), "2 ERR ARGUMENT")
        self.assertEqual(self.send("2 LOOK 181"), "2 ERR ANGLE_RANGE")
        self.assertEqual(self.send("2 LOOK -181"), "2 ERR ANGLE_RANGE")
        self.send("2 LOOK 90")
        self.assertEqual(self.send("2 LOOK 90"), "2 ERR BUSY")
        self.assertEqual(self.send("3 WAKE"), "3 ERR BUSY")
        self.send("4 STOP")
        self.c.pressed = True
        self.assertEqual(self.send("5 LOOK 90"), "5 ERR BUTTON_C")
        self.assertFalse(self.controller.ready)
        self.c.pressed = False
        self.controller.ready = True
        self.controller.voice_turn = None
        self.assertEqual(self.send("5 LOOK 90"), "5 ERR NO_GYRO")

    def test_robot_moving_during_bias_calibration_never_starts_tracks(self):
        self.send("2 LOOK 90")
        for i in range(100):
            self.gyro.last_reading_dps = (0, 0, 0 if i % 2 else 20)
            self.step()
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("2 ERR LOOK_MOVING", self.uart.outgoing)
        self.assertFalse(self.controller.ready)

    def test_slow_progress_still_has_an_absolute_deadline(self):
        self.start_motion(180)
        self.gyro.last_reading_dps = (0, 0, 5)
        for _ in range(1450): self.step()
        self.assertIn("2 ERR LOOK_TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertFalse(self.controller.ready)

    def test_acceleration_argument_and_shared_motor_ramp(self):
        self.assertIn("LOOKRAMP=1", self.send("3 STATUS"))
        for value in ("0", "499", "6001"):
            self.assertEqual(self.send("2 LOOK 90 " + value), "2 ERR ACCELERATION_RANGE")
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.send("2 LOOK 90 500"), "2 OK LOOKING")
        last = 0
        reached = False
        for _ in range(1250):
            self.step()
            speed = abs(self.motors.speeds[0])
            self.assertLessEqual(speed - last, 3)  # 500 units/s over a 5 ms tick.
            last = speed
            if speed >= MIN_TURN_SPEED:
                reached = True
                break
            self.assertIsNotNone(self.controller.look_id, "Slow ramp must not count as a stall")
        self.assertTrue(reached)
        self.assertEqual(self.controller.acceleration, 500)
        self.send("4 STOP")
        self.assertEqual(self.motors.speeds, (0, 0))
        for _ in range(30): self.step()
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_wiggle_and_voice_angles_with_motor_dead_zone(self):
        # Model the user's report: track commands up to 2000 produce no turn.
        # This checks control logic, not actual torque or heading accuracy.
        for acceleration in (500, 3000, 6000):
            for target in (15, -30, 15, 90, -90, 180):
                with self.subTest(acceleration=acceleration, target=target):
                    self.setUp()
                    self.send("2 LOOK {} {}".format(target, acceleration))
                    measured = 0.0
                    for _ in range(3500):
                        left, right = self.motors.speeds
                        rate = (right - left) / 30 if max(abs(left), abs(right)) > 2000 else 0
                        measured += rate * .005
                        self.gyro.last_reading_dps = (0, 0, rate)
                        self.step()
                        target_speed = abs(self.controller.voice_turn.command[0])
                        self.assertTrue(target_speed == 0 or MIN_TURN_SPEED <= target_speed <= MAX_TURN_SPEED)
                        if self.controller.look_id is None: break
                    self.assertIn("2 OK LOOK_DONE", self.uart.outgoing)
                    self.assertEqual(self.motors.speeds, (0, 0))
                    self.assertAlmostEqual(measured, target, delta=3.5 if abs(target) <= 30 else 8.5)

    def test_quick_wiggle_is_continuous_ramped_and_returns_to_original_heading(self):
        durations = []
        for tick_ms, response, lag_ms in ((5, 30, 0), (5, 60, 40), (20, 60, 40), (5, 120, 40)):
            for motor_sign in (1, -1):
                with self.subTest(tick=tick_ms, response=response, lag=lag_ms, motor_sign=motor_sign):
                    self.setUp()
                    self.controller.voice_turn.motor_sign = motor_sign
                    self.assertIn("WIGGLE=2", self.send("3 STATUS"))
                    self.assertEqual(self.send("2 WIGGLE"), "2 OK LOOKING")
                    started = self.clock.now
                    angle = rate = 0.0
                    low = high = 0.0
                    legs, targets = set(), set()
                    for _ in range(4500 // tick_ms + 1):
                        # Gyro preparation needs 80 samples; exercise slower
                        # control ticks only once that stationary phase ends.
                        elapsed = 5 if self.controller.voice_turn.bias is None else tick_ms
                        previous = self.motors.speeds[1]
                        requested_rate = 2 * previous * motor_sign / response if abs(previous) > 2000 else 0
                        rate += (requested_rate - rate) * (min(1, elapsed / lag_ms) if lag_ms else 1)
                        angle += rate * elapsed / 1000
                        low, high = min(low, angle), max(high, angle)
                        self.gyro.last_reading_dps = (0, 0, rate)
                        self.step(elapsed)
                        current = self.motors.speeds[1]
                        self.assertLessEqual(abs(current - previous), WIGGLE_ACCELERATION * elapsed / 1000 + 1)
                        self.assertGreaterEqual(current * previous, 0, "Reversal must pass through zero")
                        turn = self.controller.voice_turn
                        legs.add(turn.wiggle_leg)
                        targets.add(turn.command[1] * motor_sign)
                        if self.controller.look_id is None: break
                    durations.append(self.clock.now - started)
                    self.assertIn("2 OK LOOK_DONE", self.uart.outgoing)
                    self.assertEqual(legs, {0, 1, 2})
                    self.assertEqual(self.uart.outgoing.count("2 OK WIGGLE_START"), 1)
                    self.assertEqual(self.uart.outgoing.count("2 OK WIGGLE_CENTER"), 1)
                    self.assertLess(self.uart.outgoing.index("2 OK WIGGLE_START"), self.uart.outgoing.index("2 OK WIGGLE_CENTER"))
                    self.assertLess(self.uart.outgoing.index("2 OK WIGGLE_CENTER"), self.uart.outgoing.index("2 OK LOOK_DONE"))
                    self.assertEqual(self.controller.face.actions.count("show_happy"), 1)
                    self.assertEqual(self.controller.face.actions[-1], "neutral")
                    self.assertTrue({WIGGLE_SPEED, -WIGGLE_SPEED}.issubset(targets))
                    self.assertGreater(high, 7)
                    self.assertLess(low, -7)
                    self.assertAlmostEqual(angle, 0, delta=3.5)
                    self.assertLess(self.clock.now - started, 2500)
                    self.assertEqual(self.motors.speeds, (0, 0))
                    self.assertTrue(self.controller.ready)
                    # Gesture tuning must not leak into ordinary driving or braking.
                    self.assertEqual(self.send("4 DRIVE 4000 4000 500"), "4 OK DRIVE")
                    self.assertEqual(self.controller.acceleration, 500)
                    self.assertEqual(self.controller.deceleration, 12000)
        print("Quick wiggle simulation (including 400 ms gyro preparation): {}-{} ms".format(min(durations), max(durations)))

    def test_excited_wiggle_draws_before_motion_and_uses_the_same_events(self):
        self.assertEqual(self.send("2 WIGGLE EXCITED"), "2 OK LOOKING")
        while "2 OK WIGGLE_START" not in self.uart.outgoing:
            self.step(5)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.controller.face.actions[-1], ("excited", "FOUND"))
        for _ in range(6):
            self.step(5)
        self.assertNotEqual(self.motors.speeds, (0, 0))
        self.send("3 STOP")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.controller.face.actions[-1], "neutral")

    def test_greeting_face_is_drawn_before_motor_start_and_safety_rechecked_after_render(self):
        for delay in (60, 350):
            with self.subTest(display_ms=delay):
                self.setUp()
                draws = []
                def show_happy():
                    self.assertEqual(self.motors.speeds, (0, 0))
                    self.assertIsNotNone(self.controller.voice_turn.bias)
                    self.clock.now += delay
                    draws.append(self.clock.now)
                self.controller.face.show_happy = show_happy
                self.send("2 WIGGLE")
                self.assertNotIn("2 OK WIGGLE_START", self.uart.outgoing)
                for _ in range(80): self.step()
                self.assertEqual(len(draws), 1)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertIn("2 OK WIGGLE_START", self.uart.outgoing)
                self.step(5, keep=False)
                if delay == 350:
                    self.assertIsNone(self.controller.look_id)
                    self.assertEqual(self.motors.speeds, (0, 0))
                    self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)
                else:
                    self.step()
                    self.assertNotEqual(self.motors.speeds, (0, 0))
                    self.assertTrue(self.controller.ready)
                self.send("4 STOP")

    def test_greeting_display_failure_never_starts_motion_or_announces_start(self):
        def fail_display():
            raise OSError("display failed")
        self.controller.face.show_happy = fail_display
        self.send("2 WIGGLE")
        for _ in range(85): self.step()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("2 ERR LOOK_FACE", self.uart.outgoing)
        self.assertNotIn("2 OK WIGGLE_START", self.uart.outgoing)
        self.assertFalse(self.controller.ready)

    def test_quick_wiggle_cancellation_and_faults_during_each_leg(self):
        for leg in (0, 1, 2):
            for fault in ("stop", "manual", "lease", "edge", "button", "gyro"):
                with self.subTest(leg=leg, fault=fault):
                    self.setUp()
                    self.send("2 WIGGLE")
                    for _ in range(500):
                        speed = self.motors.speeds[1]
                        self.gyro.last_reading_dps = (0, 0, speed / 30 if abs(speed) > 2000 else 0)
                        self.step()
                        turn = self.controller.voice_turn
                        if turn.bias is not None and turn.wiggle_leg == leg and abs(speed) >= 2400: break
                    self.assertEqual(turn.wiggle_leg, leg)
                    self.assertIsNotNone(self.controller.look_id)
                    if fault == "stop": self.send("4 STOP")
                    elif fault == "manual": self.send("4 DRIVE 4000 4000 500")
                    elif fault == "lease": self.step(300, keep=False)
                    elif fault == "edge":
                        self.sensors.values[0] = 980
                        self.step(20)
                    elif fault == "button":
                        self.c.event = True
                        self.step()
                    else:
                        self.gyro.fail = True
                        self.step()
                    self.assertIsNone(self.controller.look_id)
                    self.assertEqual(self.motors.speeds, (0, 0))
                    self.assertEqual(self.send("50 KEEP 2"), "50 ERR NO_LOOK")
                    if fault == "manual": self.send("5 STOP")
                    for _ in range(30): self.step()
                    self.assertEqual(self.motors.speeds, (0, 0))

    def test_quick_wiggle_gates_stall_and_deadline(self):
        self.assertEqual(self.send("2 WIGGLE 6000"), "2 ERR ARGUMENT")
        self.send("2 WIGGLE")
        self.assertEqual(self.send("3 WIGGLE"), "3 ERR BUSY")
        for _ in range(300): self.step()
        self.assertIn("2 ERR LOOK_STALLED", self.uart.outgoing)
        self.assertEqual(self.send("3 WIGGLE"), "3 ERR NOT_READY")
        self.setUp()
        self.send("2 WIGGLE")
        for _ in range(900):
            if self.controller.voice_turn.bias is not None:
                self.gyro.last_reading_dps = (0, 0, 4)
            self.step()
        self.assertIn("2 ERR LOOK_TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
