"""Exercise real UART/gyro control with a simulated turn and hardware faults."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from uart_control import UARTController
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
                for _ in range(160): self.step()
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertEqual(self.controller.fault, "TURN_FAULT")
                self.assertFalse(self.controller.ready)
                self.assertIn("2 ERR " + error, self.uart.outgoing)

    def test_gates_arguments_duplicate_and_held_button(self):
        for arg in ("nan", "90.0", "1 2", ""):
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
        for _ in range(1250): self.step()
        self.assertIn("2 ERR LOOK_TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertFalse(self.controller.ready)


if __name__ == "__main__":
    unittest.main()
