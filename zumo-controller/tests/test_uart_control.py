"""Desktop checks for the UART protocol and motor safety state machine."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from uart_control import UARTController


class Clock:
    def __init__(self):
        self.now = 0

    def ticks_ms(self):
        return self.now

    def ticks_diff(self, newer, older):
        return newer - older

    def ticks_add(self, value, delta):
        return value + delta


class UART:
    def __init__(self):
        self.incoming = bytearray()
        self.outgoing = []
        self.closed = False

    def read(self, size):
        data = bytes(self.incoming[:size])
        del self.incoming[:size]
        return data or None

    def write(self, data):
        self.outgoing.append(data.decode().rstrip("\n"))

    def deinit(self):
        self.closed = True

    def send(self, command):
        self.incoming.extend((command + "\n").encode())


class Motors:
    def __init__(self):
        self.speeds = (0, 0)

    def off(self):
        self.speeds = (0, 0)

    def set_speeds(self, left, right):
        self.speeds = (left, right)


class Sensors:
    def __init__(self):
        self.values = [100] * 5
        self.fail = False

    def read(self):
        if self.fail:
            raise OSError("sensor failure")
        return self.values


class Battery:
    def get_level_millivolts(self):
        return 7600


class Face:
    def __init__(self):
        self.actions = []

    def play_happy(self):
        self.actions.append("happy")

    def play_sad(self):
        self.actions.append("sad")

    def update(self):
        pass


class Button:
    def __init__(self):
        self.pressed = False
        self.event = False

    def is_pressed(self):
        return self.pressed

    def check(self):
        event, self.event = self.event, False
        return True if event else None


class UARTControlTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.uart = UART()
        self.motors = Motors()
        self.sensors = Sensors()
        self.face = Face()
        self.a, self.b, self.c = Button(), Button(), Button()
        self.control = UARTController(
            self.uart, self.motors, self.sensors, Battery(), self.face,
            self.a, self.b, self.c, self.clock,
        )

    def command(self, text):
        self.uart.send(text)
        self.control.tick()
        return self.uart.outgoing[-1]

    def calibrate(self):
        self.assertEqual(self.command("1 CALIBRATE"), "1 OK CALIBRATING")
        for _ in range(60):
            self.clock.now += 10
            self.control.tick()
        self.assertEqual(self.uart.outgoing[-1], "0 EVENT READY")

    def test_protocol_and_calibration_gate(self):
        self.assertEqual(self.command("2 PING"), "2 OK PONG")
        self.assertEqual(self.command("3 DRIVE 1000 1000"), "3 ERR NOT_READY")
        self.calibrate()
        status = self.command("4 STATUS")
        self.assertIn("READY=1 FAULT=NONE", status)
        self.assertIn("BAT=7600 EDGE=0", status)
        self.assertEqual(self.command("5 FACE HAPPY"), "5 OK FACE")
        self.assertEqual(self.face.actions, ["happy"])
        self.assertEqual(self.command("6 FACE SAD"), "6 OK FACE")

    def test_drive_limits_stop_and_timeout(self):
        self.calibrate()
        self.assertEqual(self.command("0 PING"), "0 ERR ID_RANGE")
        self.assertEqual(self.command("2 DRIVE --3 0"), "2 ERR ARGUMENT")
        self.assertEqual(self.command("2 DRIVE 2001 0"), "2 ERR SPEED_RANGE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("3 DRIVE 1000 -900"), "3 OK DRIVE")
        self.assertEqual(self.motors.speeds, (1000, -900))
        self.clock.now += 250
        self.assertEqual(self.command("4 PING"), "4 OK PONG")
        self.clock.now += 50
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.command("5 DRIVE -2000 2000"), "5 OK DRIVE")
        self.assertEqual(self.command("6 STOP"), "6 OK STOPPED")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("7 STATUS").split(" L=")[1][:7], "0 R=0 B")

    def test_refresh_keeps_drive_alive_and_face_waits_for_stop(self):
        self.calibrate()
        for index in range(4):
            self.clock.now += 100
            self.assertEqual(self.command("{} DRIVE 700 700".format(index + 2)),
                             "{} OK DRIVE".format(index + 2))
        self.a.event = True
        self.control.tick()
        self.assertEqual(self.face.actions, [])
        self.assertEqual(self.command("7 FACE HAPPY"), "7 ERR MOVING")
        self.assertEqual(self.motors.speeds, (700, 700))
        self.command("8 STOP")
        self.b.event = True
        self.control.tick()
        self.assertEqual(self.face.actions, ["happy"])

    def test_edge_latches_until_recalibrated(self):
        self.calibrate()
        self.command("2 DRIVE 1000 1000")
        self.sensors.values = [900, 100, 100, 100, 100]
        self.clock.now += 20
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.control.fault, "EDGE")
        self.assertEqual(self.command("3 DRIVE -500 -500"), "3 ERR NOT_READY")
        self.assertIn("EDGE=1", self.command("4 STATUS"))
        self.sensors.values = [600, 100, 100, 100, 100]
        self.command("5 CALIBRATE")
        for _ in range(60):
            self.clock.now += 10
            self.control.tick()
        self.assertEqual(self.uart.outgoing[-1], "0 EVENT CAL_FAILED")
        self.sensors.values = [100] * 5
        self.calibrate()
        self.assertEqual(self.command("5 DRIVE 500 500"), "5 OK DRIVE")

    def test_button_c_and_sensor_fault(self):
        self.calibrate()
        self.command("2 DRIVE 500 500")
        self.c.event = True
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("3 DRIVE 500 500"), "3 ERR NOT_READY")
        self.calibrate()
        self.sensors.fail = True
        self.assertEqual(self.command("4 DRIVE 500 500"), "4 ERR SENSOR")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_stop_interrupts_calibration_and_rejects_bad_lines(self):
        self.command("1 CALIBRATE")
        self.assertEqual(self.command("2 STOP"), "2 OK STOPPED")
        self.assertEqual(self.command("3 DRIVE 500 500"), "3 ERR NOT_READY")
        self.assertEqual(self.command("no-id PING"), "0 ERR FORMAT")
        self.assertEqual(self.command("4 DRIVE nope 0"), "4 ERR ARGUMENT")
        self.assertEqual(self.command("5" + "x" * 70), "0 ERR LINE_TOO_LONG")
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_failed_calibration_does_not_arm_motors(self):
        self.sensors.values = [900] * 5
        self.command("1 CALIBRATE")
        for _ in range(60):
            self.clock.now += 10
            self.control.tick()
        self.assertEqual(self.uart.outgoing[-1], "0 EVENT CAL_FAILED")
        self.assertEqual(self.command("2 DRIVE 500 500"), "2 ERR NOT_READY")
        self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
