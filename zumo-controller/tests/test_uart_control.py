"""Desktop checks for the UART protocol and motor safety state machine."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from uart_control import UARTController
from digit_face_engine import EXPRESSIONS


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

    def show_happy(self):
        self.actions.append("show_happy")

    def show_scene(self, expression, caption):
        self.actions.append((expression, caption))

    def play(self, expression, hold_ms=None):
        self.actions.append(expression)

    def play_sad(self):
        self.actions.append("sad")

    def update(self):
        pass


class RGBLEDs:
    def __init__(self):
        self.colors = [[0, 0, 0] for _ in range(6)]
        self.brightness = [5] * 6
        self.shows = 0

    def set(self, index, color):
        self.colors[index] = list(color)

    def set_brightness(self, value, led):
        self.brightness[led] = value

    def show(self):
        self.shows += 1

    def get(self, index):
        return self.colors[index]

    def get_brightness(self, index):
        return self.brightness[index]


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
    def test_scene_is_stationary_validated_and_stoppable(self):
        self.assertEqual(self.command("2 SCENE HAPPY BATTERY"), "2 OK SCENE")
        self.assertEqual(self.face.actions[-1], ("happy", "BATTERY"))
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("3 SCENE HAPPY GARAGE"), "3 ERR ARGUMENT")
        self.assertEqual(self.command("4 SCENE happy BATTERY"), "4 ERR ARGUMENT")
        self.assertEqual(self.command("5 STOP"), "5 OK STOPPED")
        self.assertEqual(self.face.actions[-1], "neutral")
        self.calibrate()
        self.command("6 DRIVE 2800 2800 6000")
        self.assertEqual(self.command("7 SCENE EXCITED FOUND"), "7 ERR MOVING")

    def setUp(self):
        self.clock = Clock()
        self.uart = UART()
        self.motors = Motors()
        self.sensors = Sensors()
        self.face = Face()
        self.leds = RGBLEDs()
        self.a, self.b, self.c = Button(), Button(), Button()
        self.control = UARTController(
            self.uart, self.motors, self.sensors, Battery(), self.face,
            self.a, self.b, self.c, self.clock, self.leds,
        )

    def command(self, text):
        self.uart.send(text)
        self.control.tick()
        return self.uart.outgoing[-1]

    def advance(self, ms, refresh=None):
        remaining = ms
        while remaining:
            delta = min(5, remaining)
            self.clock.now += delta
            remaining -= delta
            if refresh and self.clock.now % 100 == 0:
                self.uart.send(refresh)
            self.control.tick()

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

    def test_removed_bump_commands_cannot_move(self):
        self.calibrate()
        self.assertNotIn("BUMP=", self.command("2 STATUS"))
        for command in ("BUMP 1", "BUMP -1", "BKEEP 2"):
            self.assertEqual(self.command("3 " + command), "3 ERR UNKNOWN_COMMAND")
            self.assertEqual(self.motors.speeds, (0, 0))
        self.assertTrue(self.control.ready)

    def test_startup_calibration_is_stationary_and_accepts_late_xiao(self):
        self.control.start_calibration()
        self.assertEqual(self.uart.outgoing[-1], "0 OK CALIBRATING")
        self.assertEqual(self.command("2 DRIVE 1000 1000"), "2 ERR NOT_READY")
        self.advance(580)
        self.assertFalse(self.control.ready)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(20)
        self.assertTrue(self.control.ready)
        self.assertEqual(self.uart.outgoing[-1], "0 EVENT READY")
        self.advance(60000)
        self.assertIn("READY=1 FAULT=NONE", self.command("3 STATUS"))
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_startup_calibration_failure_stays_locked_until_manual_retry(self):
        for failure in ("baseline", "sensor"):
            with self.subTest(failure=failure):
                self.setUp()
                self.sensors.values = [900] * 5 if failure == "baseline" else [100] * 5
                self.sensors.fail = failure == "sensor"
                self.control.start_calibration()
                self.advance(600)
                self.assertEqual(self.control.fault, "CAL_FAILED" if failure == "baseline" else "SENSOR")
                self.sensors.values = [100] * 5
                self.sensors.fail = False
                self.advance(1000)
                self.assertFalse(self.control.ready)
                self.assertIsNone(self.control.calibration)
                self.assertEqual(self.command("2 DRIVE 500 500"), "2 ERR NOT_READY")
                self.assertEqual(self.motors.speeds, (0, 0))
                self.calibrate()

    def test_stop_cancels_startup_calibration_without_automatic_retry(self):
        self.control.start_calibration()
        self.advance(200)
        self.assertEqual(self.command("2 STOP"), "2 OK STOPPED")
        self.advance(1000)
        self.assertFalse(self.control.ready)
        self.assertIsNone(self.control.calibration)
        self.assertEqual(self.command("3 DRIVE 500 500"), "3 ERR NOT_READY")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.calibrate()

    def test_button_c_blocks_or_cancels_startup_calibration(self):
        for held_at_start in (True, False):
            with self.subTest(held_at_start=held_at_start):
                self.setUp()
                self.c.pressed = held_at_start
                self.control.start_calibration()
                if held_at_start:
                    self.assertEqual(self.uart.outgoing[-1], "0 ERR BUTTON_C")
                else:
                    self.advance(200)
                    self.c.event = True
                    self.control.tick()
                    self.assertEqual(self.control.fault, "BUTTON_C")
                self.c.pressed = False
                self.advance(1000)
                self.assertFalse(self.control.ready)
                self.assertIsNone(self.control.calibration)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.calibrate()

    def test_led_control_works_without_arming_motors(self):
        self.assertEqual(self.command("2 LED ALL 12 34 56 5"), "2 OK LED")
        self.assertEqual(self.leds.colors, [[12, 34, 56]] * 6)
        self.assertEqual(self.leds.brightness, [5] * 6)
        self.assertEqual(self.command("3 LED 2 255 0 90 12"), "3 OK LED")
        self.assertEqual(self.leds.colors[2], [255, 0, 90])
        self.assertEqual(self.leds.brightness[2], 12)
        self.assertEqual(self.command("4 LED 2 0 0 0 0"), "4 OK LED")
        self.assertEqual(self.leds.colors[2], [0, 0, 0])
        self.assertEqual(self.command("5 LED 6 1 2 3 5"), "5 ERR ARGUMENT")
        self.assertEqual(self.command("6 LED ALL 256 2 3 5"), "6 ERR LED_RANGE")
        self.assertEqual(self.command("7 LED ALL 1 2 3 32"), "7 ERR LED_RANGE")
        self.assertEqual(self.leds.shows, 3)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertFalse(self.control.ready)

    def test_all_faces_work_unarmed_and_reject_unknown_or_removed_names(self):
        for name in EXPRESSIONS:
            self.assertEqual(self.command("2 FACE " + name.upper()), "2 OK FACE")
            self.assertEqual(self.face.actions[-1], name)
            self.assertFalse(self.control.ready)
            self.assertEqual(self.motors.speeds, (0, 0))
        before = list(self.face.actions)
        for name in ("DIZZY", "UNKNOWN", "happy", "SAD EXTRA"):
            self.assertEqual(self.command("3 FACE " + name), "3 ERR ARGUMENT")
        self.assertEqual(self.face.actions, before)

    def test_every_face_is_rejected_while_moving(self):
        self.calibrate()
        self.command("2 DRIVE 700 700")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(240)
        for name in EXPRESSIONS:
            self.assertEqual(self.command("3 FACE " + name.upper()), "3 ERR MOVING")
            self.assertEqual(self.motors.speeds, (700, 700))
        self.assertEqual(self.face.actions, [])

    def test_xiao_can_start_long_after_zumo(self):
        # The Zumo has no startup connection deadline. A late peer must be
        # able to discover it through the XIAO's periodic STATUS request.
        self.clock.now = 60000
        self.assertEqual(self.command("7 PING"), "7 OK PONG")
        self.assertIn("READY=0 FAULT=NONE", self.command("8 STATUS"))
        self.calibrate()
        self.assertEqual(self.command("9 DRIVE 4000 4000"), "9 OK DRIVE")

    def test_ramp_reaches_requested_speed_without_refresh_restarting_it(self):
        self.calibrate()
        self.clock.now += 60000  # An idle delay must not create a startup jump.
        self.command("2 DRIVE 4000 -2000 3000")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(100, "2 DRIVE 4000 -2000 3000")
        self.assertEqual(self.motors.speeds, (300, -300))
        self.advance(900, "2 DRIVE 4000 -2000 3000")
        self.assertEqual(self.motors.speeds, (3000, -2000))
        self.advance(500, "2 DRIVE 4000 -2000 3000")
        self.assertEqual(self.motors.speeds, (4000, -2000))
        self.assertIn("L=4000 R=-2000", self.command("3 STATUS"))

    def test_acceleration_validation_and_live_change(self):
        self.calibrate()
        for value in ("0", "499", "6001", "-500"):
            self.assertEqual(self.command("2 DRIVE 4000 4000 " + value),
                             "2 ERR ACCEL_RANGE")
        for value in ("--500", "fast", "500 extra"):
            self.assertEqual(self.command("2 DRIVE 4000 4000 " + value),
                             "2 ERR ARGUMENT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.command("2 DRIVE 4000 4000 500")
        self.advance(100)
        self.assertEqual(self.motors.speeds, (50, 50))
        self.command("3 DRIVE 4000 4000 6000")
        self.advance(100)
        self.assertEqual(self.motors.speeds, (650, 650))

    def test_brake_reverse_and_stop_clear_the_ramp(self):
        self.calibrate()
        self.command("2 DRIVE 2000 2000")
        self.advance(500, "2 DRIVE 2000 2000")
        self.assertEqual(self.motors.speeds, (1500, 1500))
        self.command("3 DRIVE 500 500")
        self.assertEqual(self.motors.speeds, (1500, 1500))
        self.advance(100)
        self.assertEqual(self.motors.speeds, (500, 500))
        self.command("4 DRIVE -2000 -2000")
        self.assertEqual(self.motors.speeds, (500, 500))
        self.advance(45)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(100)
        self.assertEqual(self.motors.speeds, (-300, -300))
        self.command("5 DRIVE 0 0")
        self.assertEqual(self.motors.speeds, (-300, -300))
        self.advance(25)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.command("6 DRIVE 4000 4000")
        self.advance(100)
        self.command("7 STOP")
        self.advance(1000)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual((self.control.target_left, self.control.target_right), (0, 0))
        self.command("8 DRIVE 4000 4000")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(5)
        self.assertEqual(self.motors.speeds, (15, 15))

    def test_pending_ramp_is_busy_and_safety_cannot_restart_it(self):
        for fault in ("EDGE", "BUTTON_C", "SENSOR", "TIMEOUT"):
            with self.subTest(fault=fault):
                self.setUp()
                self.calibrate()
                self.command("2 DRIVE 4000 4000 500")
                self.assertEqual(self.command("3 CALIBRATE"), "3 ERR MOVING")
                self.assertEqual(self.command("4 FACE HAPPY"), "4 ERR MOVING")
                self.assertEqual(self.command("5 WAKE"), "5 ERR MOVING")
                if fault == "EDGE":
                    self.sensors.values[0] = 980
                elif fault == "BUTTON_C":
                    self.c.event = True
                elif fault == "SENSOR":
                    self.sensors.fail = True
                self.advance(300 if fault == "TIMEOUT" else 20)
                self.assertEqual(self.motors.speeds, (0, 0))
                self.assertEqual((self.control.target_left, self.control.target_right), (0, 0))
                self.advance(500)
                self.assertEqual(self.motors.speeds, (0, 0))

    def test_slow_loop_cannot_jump_to_target_speed(self):
        self.calibrate()
        self.command("2 DRIVE 6000 6000 6000")
        self.clock.now += 200
        self.control.tick()
        self.assertEqual(self.motors.speeds, (120, 120))

    def test_ramp_and_watchdog_across_clock_wrap(self):
        self.calibrate()
        self.clock.now = 65530
        self.clock.ticks_ms = lambda: self.clock.now % 65536
        self.clock.ticks_diff = lambda a, b: (a - b + 32768) % 65536 - 32768
        self.command("2 DRIVE 4000 4000")
        self.advance(100)
        self.assertEqual(self.motors.speeds, (300, 300))
        self.advance(200)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)

    def test_drive_limits_stop_and_timeout(self):
        self.calibrate()
        self.assertEqual(self.command("0 PING"), "0 ERR ID_RANGE")
        self.assertEqual(self.command("2 DRIVE --3 0"), "2 ERR ARGUMENT")
        self.assertEqual(self.command("2 DRIVE 6001 0"), "2 ERR SPEED_RANGE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("3 DRIVE 1000 -900"), "3 OK DRIVE")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(250)
        self.assertEqual(self.motors.speeds, (750, -750))
        self.assertEqual(self.command("4 PING"), "4 OK PONG")
        self.clock.now += 50
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.command("5 DRIVE -6000 6000"), "5 OK DRIVE")
        self.assertEqual(self.command("6 STOP"), "6 OK STOPPED")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("7 STATUS").split(" L=")[1][:7], "0 R=0 B")

    def test_turn_is_rotation_only_and_keeps_safety_rules(self):
        self.assertEqual(self.command("2 TURN 4000"), "2 ERR NOT_READY")
        self.calibrate()
        self.assertEqual(self.command("3 TURN 6001"), "3 ERR SPEED_RANGE")
        self.assertEqual(self.command("4 TURN --2"), "4 ERR ARGUMENT")
        self.assertEqual(self.command("5 TURN 4000 0"), "5 ERR ARGUMENT")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("6 TURN -4000"), "6 OK TURN")
        self.assertEqual(self.motors.speeds, (0, 0))
        self.advance(299)
        self.assertIn("L=-1794 R=1794 BAT=", self.command("7 STATUS"))
        self.clock.now += 1
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIn("0 EVENT TIMEOUT", self.uart.outgoing)
        self.assertEqual(self.command("8 TURN 4000"), "8 OK TURN")
        self.advance(20)
        self.assertEqual(self.motors.speeds, (120, -120))
        self.sensors.values = [980, 100, 100, 100, 100]
        self.clock.now += 20
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.command("9 TURN 4000"), "9 ERR NOT_READY")

    def test_refresh_keeps_drive_alive_and_face_waits_for_stop(self):
        self.calibrate()
        for index in range(4):
            self.advance(100)
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
        self.assertEqual(self.face.actions, ["neutral", "happy"])

    def test_edge_latches_until_recalibrated(self):
        self.calibrate()
        self.command("2 DRIVE 1000 1000")
        self.sensors.values = [980, 100, 100, 100, 100]
        self.clock.now += 20
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertEqual(self.control.fault, "EDGE")
        self.assertEqual(self.command("3 DRIVE -500 -500"), "3 ERR NOT_READY")
        self.assertIn("EDGE=1", self.command("4 STATUS"))
        self.sensors.values = [650, 100, 100, 100, 100]
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

    def test_wake_runs_unarmed_and_restores_each_led(self):
        self.command("1 LED ALL 12 34 56 4")
        self.command("2 LED 3 90 80 70 9")
        colors = [list(color) for color in self.leds.colors]
        brightness = list(self.leds.brightness)
        self.assertEqual(self.command("3 WAKE"), "3 OK WAKE")
        self.assertEqual(self.face.actions[-1], "happy")
        self.assertEqual(self.command("4 WAKE"), "4 ERR BUSY")
        self.clock.now += 600
        self.control.tick()
        self.assertNotEqual(self.leds.colors, colors)
        self.assertEqual(self.command("5 PING"), "5 OK PONG")
        self.clock.now += 1700
        self.control.tick()
        self.assertEqual(self.leds.colors, colors)
        self.assertEqual(self.leds.brightness, brightness)
        self.assertFalse(self.control.wake.active)
        self.assertFalse(self.control.ready)
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_wake_respects_moving_calibrating_and_button_c(self):
        self.command("1 CALIBRATE")
        self.assertEqual(self.command("2 WAKE"), "2 ERR BUSY")
        self.command("3 STOP")
        self.calibrate()
        self.command("4 DRIVE 1000 1000")
        self.advance(100)
        self.assertEqual(self.command("5 WAKE"), "5 ERR MOVING")
        self.assertEqual(self.motors.speeds, (300, 300))
        self.command("6 STOP")
        self.c.pressed = True
        self.assertEqual(self.command("7 WAKE"), "7 ERR BUTTON_C")
        self.assertEqual(self.face.actions, ["neutral", "neutral"])
        self.assertEqual(self.command("8 WAKE HAPPY"), "8 ERR ARGUMENT")

    def test_manual_override_is_not_undone_when_animation_expires(self):
        self.command("1 WAKE")
        self.command("2 LED 2 11 22 33 8")
        self.assertFalse(self.control.wake.active)
        self.clock.now += 3000
        self.control.tick()
        self.assertEqual(self.leds.colors[2], [11, 22, 33])
        self.assertEqual(self.leds.brightness[2], 8)
        self.command("3 WAKE")
        self.command("4 FACE SLEEPING")
        self.clock.now += 3000
        self.control.tick()
        self.assertEqual(self.face.actions[-1], "sleeping")

    def test_wake_cannot_delay_stop_edge_or_drive_timeout(self):
        self.calibrate()
        self.command("2 WAKE")
        self.c.event = True
        self.control.tick()
        self.assertFalse(self.control.wake.active)
        self.assertEqual(self.control.fault, "BUTTON_C")
        self.assertFalse(self.control.ready)
        self.calibrate()
        self.command("3 WAKE")
        self.sensors.values = [980, 100, 100, 100, 100]
        self.clock.now += 20
        self.control.tick()
        self.assertEqual(self.control.fault, "EDGE")
        self.assertFalse(self.control.wake.active)
        self.sensors.values = [100] * 5
        self.calibrate()
        self.command("4 WAKE")
        self.command("5 DRIVE 700 700")
        self.assertFalse(self.control.wake.active)
        self.clock.now += 300
        self.control.tick()
        self.assertEqual(self.motors.speeds, (0, 0))

    def test_led_failure_during_wake_does_not_break_control(self):
        self.command("1 WAKE")
        def fail():
            raise OSError("LED bus failure")
        self.leds.show = fail
        self.clock.now += 100
        self.control.tick()
        self.assertFalse(self.control.wake.active)
        self.assertIn("0 EVENT WAKE_FAILED", self.uart.outgoing)
        self.assertEqual(self.command("2 STOP"), "2 OK STOPPED")
        self.assertEqual(self.command("3 PING"), "3 OK PONG")
        self.assertEqual(self.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
