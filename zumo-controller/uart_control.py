"""Motor-safe, line-oriented UART control for Digit's Zumo 2040.

The XIAO sends ``<id> COMMAND [arguments]\n`` on UART0. This module has no
hardware imports so its timing and safety rules can be tested on a desktop.
"""


from digit_face_engine import EXPRESSIONS
from wake_reaction import WakeReaction
from voice_turn import VoiceTurn


MAX_LINE = 63
MAX_DRIVE_SPEED = 6000
DRIVE_TIMEOUT_MS = 300
EDGE_POLL_MS = 20
CALIBRATION_INTERVAL_MS = 10
CALIBRATION_SAMPLES = 60
MAX_SURFACE_BASELINE = 850
MIN_EDGE_MARGIN = 180


class UARTController:
    def __init__(self, uart, motors, line_sensors, battery, face,
                 button_a, button_b, button_c, clock, rgb_leds=None,
                 gyro=None, turn_motor_sign=1, tof=None):
        self.uart = uart
        self.motors = motors
        self.line_sensors = line_sensors
        self.battery = battery
        self.face = face
        self.rgb_leds = rgb_leds
        self.button_a = button_a
        self.button_b = button_b
        self.button_c = button_c
        self.clock = clock
        self.tof = tof
        self.distance_mm = -1
        self.range_at = None
        self.last_range_poll = None
        self.range_failed = False
        self.safe_motion = False
        self.wake = WakeReaction(face, rgb_leds, clock)
        self.voice_turn = VoiceTurn(gyro, clock, turn_motor_sign) if gyro else None
        self.look_id = None
        self.last_keep = None
        self.line = bytearray()
        self.dropping = False
        self.ready = False
        self.fault = "NONE"
        self.edge = False
        self.thresholds = None
        self.blocked_thresholds = None
        self.left = 0
        self.right = 0
        self.last_drive = None
        self.last_edge_poll = None
        self.calibration = None
        self.motors.off()

    def _send(self, value):
        self.uart.write((value + "\n").encode())

    def _stop(self):
        self.motors.off()
        self.left = 0
        self.right = 0
        self.last_drive = None
        self.safe_motion = False
        if self.look_id is not None:
            self._send(self.look_id + " ERR LOOK_CANCELLED")
        self.look_id = None
        self.last_keep = None
        if self.voice_turn:
            self.voice_turn.cancel()
        self._cancel_wake()

    def _cancel_wake(self):
        try:
            self.wake.cancel()
        except Exception as error:
            print("UART CONTROL: wake restore error:", error)
            self._send("0 EVENT WAKE_FAILED")

    def _latch(self, reason):
        self._stop()
        self.ready = False
        if reason == "EDGE":
            self.blocked_thresholds = self.thresholds
        self.thresholds = None
        self.calibration = None
        self.fault = reason
        self.edge = reason == "EDGE"
        self._send("0 EVENT " + reason)
        print("UART CONTROL: motor stop,", reason)

    def _read_edge(self):
        try:
            readings = self.line_sensors.read()
            if len(readings) != 5:
                raise ValueError("expected five line readings")
            return readings
        except Exception as error:
            print("UART CONTROL: line sensor error:", error)
            self._latch("SENSOR")
            return None

    def _edge_seen(self, readings):
        return any(readings[i] >= self.thresholds[i] for i in range(5))

    def _start_calibration(self, command_id, now):
        if self.calibration is not None:
            self._send(command_id + " ERR BUSY")
            return
        if self.left or self.right:
            self._send(command_id + " ERR MOVING")
            return
        if self.button_c.is_pressed():
            self._send(command_id + " ERR BUTTON_C")
            return
        self._stop()
        self.ready = False
        self.thresholds = None
        self.calibration = {
            "count": 0,
            "totals": [0] * 5,
            "next": now,
        }
        self._send(command_id + " OK CALIBRATING")
        print("UART CONTROL: calibrating on stationary floor")

    def _sample_calibration(self, now):
        state = self.calibration
        if state is None or self.clock.ticks_diff(now, state["next"]) < 0:
            return
        readings = self._read_edge()
        if readings is None:
            return
        for i in range(5):
            state["totals"][i] += readings[i]
        state["count"] += 1
        state["next"] = self.clock.ticks_add(now, CALIBRATION_INTERVAL_MS)
        if state["count"] < CALIBRATION_SAMPLES:
            return
        baselines = [total // CALIBRATION_SAMPLES for total in state["totals"]]
        self.calibration = None
        if (any(value >= MAX_SURFACE_BASELINE for value in baselines) or
                (self.blocked_thresholds is not None and
                 any(baselines[i] >= self.blocked_thresholds[i]
                     for i in range(5)))):
            self.fault = "CAL_FAILED"
            self._send("0 EVENT CAL_FAILED")
            print("UART CONTROL: calibration rejected:", baselines)
            return
        self.thresholds = [
            min(975, value + max(MIN_EDGE_MARGIN, (1024 - value) // 2))
            for value in baselines
        ]
        self.fault = "NONE"
        self.edge = False
        self.blocked_thresholds = None
        self.ready = True
        self.last_edge_poll = now
        self._send("0 EVENT READY")
        print("UART CONTROL: edge thresholds:", self.thresholds)

    def _status(self, command_id):
        try:
            millivolts = self.battery.get_level_millivolts()
        except Exception as error:
            print("UART CONTROL: battery read error:", error)
            millivolts = -1
        valid = self._range_valid(self.clock.ticks_ms())
        self._send("{} OK READY={} FAULT={} L={} R={} BAT={} EDGE={} VOICE={} DIST={} RANGE={} AUTO=1".format(
            command_id, int(self.ready), self.fault, self.left, self.right,
            millivolts, int(self.edge), int(self.voice_turn is not None),
            self.distance_mm, int(valid)))

    def _range_valid(self, now):
        return (self.tof is not None and self.range_at is not None and
                self.clock.ticks_diff(now, self.range_at) < 250 and
                self.distance_mm > 0)

    def _poll_range(self, now):
        if self.tof is None or (self.last_range_poll is not None and
                self.clock.ticks_diff(now, self.last_range_poll) < 20):
            return
        self.last_range_poll = now
        try:
            if self.tof.data_ready:
                distance = self.tof.distance_mm
                valid = self.tof.range_status == 0
                self.tof.clear_interrupt()
                self.distance_mm = distance if valid else -1
                self.range_at = now if valid else None
                self.range_failed = False
        except Exception as error:
            self.distance_mm = -1
            self.range_at = None
            if self.safe_motion:
                self._latch("SENSOR")
            if not self.range_failed:
                print("UART CONTROL: range sensor error:", error)
            self.range_failed = True

    def _apply_motion(self, command_id, left, right, now, reply):
        if not self.ready:
            self._send(command_id + " ERR NOT_READY")
            return
        readings = self._read_edge()
        if readings is None or self._edge_seen(readings):
            if readings is not None:
                self._latch("EDGE")
            self._send(command_id + " ERR " + self.fault)
            return
        if self.look_id is not None:
            self._stop()  # Manual motion supersedes an automatic voice turn.
        else:
            self._cancel_wake()
        self.motors.set_speeds(left, right)
        self.safe_motion = False
        self.left, self.right = left, right
        self.last_drive = now if left or right else None
        self._send(command_id + " OK " + reply)

    def _handle_line(self, raw, now):
        try:
            line = raw.decode("ascii")
        except UnicodeError:
            self._send("0 ERR FORMAT")
            return
        words = line.split()
        if len(words) < 2 or not words[0].isdigit():
            self._send("0 ERR FORMAT")
            return
        command_id = words[0]
        if not 1 <= int(command_id) <= 65535:
            self._send("0 ERR ID_RANGE")
            return
        command = words[1]
        args = words[2:]
        if command == "PING" and not args:
            self._send(command_id + " OK PONG")
        elif command == "STATUS" and not args:
            self._status(command_id)
        elif command == "STOP" and not args:
            self._stop()
            self.calibration = None
            self.face.play("neutral")
            self._send(command_id + " OK STOPPED")
        elif command == "CALIBRATE" and not args:
            self._start_calibration(command_id, now)
        elif command == "LOOK" and len(args) == 1:
            argument = args[0]
            if not (argument[1:] if argument.startswith("-") else argument).isdigit():
                self._send(command_id + " ERR ARGUMENT")
            elif not -180 <= int(argument) <= 180:
                self._send(command_id + " ERR ANGLE_RANGE")
            elif not self.ready:
                self._send(command_id + " ERR NOT_READY")
            elif self.button_c.is_pressed():
                self._latch("BUTTON_C")
                self._send(command_id + " ERR BUTTON_C")
            elif self.voice_turn is None:
                self._send(command_id + " ERR NO_GYRO")
            elif self.look_id is not None or self.left or self.right or self.calibration is not None:
                self._send(command_id + " ERR BUSY")
            else:
                readings = self._read_edge()
                if readings is None or self._edge_seen(readings):
                    if readings is not None:
                        self._latch("EDGE")
                    self._send(command_id + " ERR " + self.fault)
                else:
                    self._stop()
                    self.look_id = command_id
                    self.last_keep = now
                    self.voice_turn.start(int(argument), now)
                    self._send(command_id + " OK LOOKING")
        elif command == "KEEP" and len(args) == 1:
            # Only the original LOOK id can refresh its 300 ms lease.
            if self.look_id is not None and args[0] == self.look_id:
                self.last_keep = now
                self._send(command_id + " OK KEEP")
            else:
                self._send(command_id + " ERR NO_LOOK")
        elif command in ("DRIVE", "SAFE") and len(args) == 2:
            if not all((arg[1:] if arg.startswith("-") else arg).isdigit()
                       for arg in args):
                self._send(command_id + " ERR ARGUMENT")
                return
            left, right = int(args[0]), int(args[1])
            if not (-MAX_DRIVE_SPEED <= left <= MAX_DRIVE_SPEED and
                    -MAX_DRIVE_SPEED <= right <= MAX_DRIVE_SPEED):
                self._send(command_id + " ERR SPEED_RANGE")
            elif command == "SAFE" and not self._range_valid(now):
                self._stop()
                self._send(command_id + " ERR RANGE_UNAVAILABLE")
            elif command == "SAFE" and left + right > 0 and self.distance_mm <= 150:
                self._stop()
                self._send(command_id + " ERR OBSTACLE")
            else:
                self._apply_motion(command_id, left, right, now, command)
                self.safe_motion = command == "SAFE" and bool(self.left or self.right)
        elif command == "TURN" and len(args) == 1:
            argument = args[0]
            if not (argument[1:] if argument.startswith("-") else argument).isdigit():
                self._send(command_id + " ERR ARGUMENT")
                return
            speed = int(argument)
            if not -MAX_DRIVE_SPEED <= speed <= MAX_DRIVE_SPEED:
                self._send(command_id + " ERR SPEED_RANGE")
            else:
                # Equal and opposite tracks enforce rotation at the robot.
                # Positive turns right; negative turns left.
                self._apply_motion(command_id, speed, -speed, now, "TURN")
        elif command == "WAKE" and not args:
            if self.left or self.right:
                self._send(command_id + " ERR MOVING")
            elif self.calibration is not None or self.wake.active or self.look_id is not None:
                self._send(command_id + " ERR BUSY")
            elif self.button_c.is_pressed():
                self._send(command_id + " ERR BUTTON_C")
            elif self.rgb_leds is None:
                self._send(command_id + " ERR LED_UNAVAILABLE")
            else:
                try:
                    self.wake.start(now)
                except Exception as error:
                    self._cancel_wake()
                    print("UART CONTROL: wake reaction error:", error)
                    self._send(command_id + " ERR WAKE_IO")
                else:
                    self._send(command_id + " OK WAKE")
        elif command == "FACE" and len(args) == 1:
            if self.left or self.right or self.look_id is not None:
                self._send(command_id + " ERR MOVING")
            elif args[0] == args[0].upper() and args[0].lower() in EXPRESSIONS:
                self._cancel_wake()
                self.face.play(args[0].lower())
                self._send(command_id + " OK FACE")
            else:
                self._send(command_id + " ERR ARGUMENT")
        elif command == "LED" and len(args) == 5:
            target = args[0]
            values = args[1:]
            if (target != "ALL" and not (target.isdigit() and 0 <= int(target) < 6)) or not all(value.isdigit() for value in values):
                self._send(command_id + " ERR ARGUMENT")
            else:
                red, green, blue, brightness = (int(value) for value in values)
                if any(value > 255 for value in (red, green, blue)) or brightness > 31:
                    self._send(command_id + " ERR LED_RANGE")
                elif self.rgb_leds is None:
                    self._send(command_id + " ERR LED_UNAVAILABLE")
                else:
                    try:
                        self._cancel_wake()
                        for index in range(6) if target == "ALL" else (int(target),):
                            self.rgb_leds.set_brightness(brightness, led=index)
                            self.rgb_leds.set(index, (red, green, blue))
                        self.rgb_leds.show()
                    except Exception as error:
                        print("UART CONTROL: RGB LED error:", error)
                        self._send(command_id + " ERR LED_IO")
                    else:
                        self._send(command_id + " OK LED")
        elif command in ("PING", "STATUS", "STOP", "CALIBRATE", "DRIVE", "SAFE", "TURN", "FACE", "LED", "WAKE", "LOOK", "KEEP"):
            self._send(command_id + " ERR ARGUMENT")
        else:
            self._send(command_id + " ERR UNKNOWN_COMMAND")

    def _poll_uart(self, now):
        data = self.uart.read(128)
        if not data:
            return
        for char in data:
            if char == 13:
                continue
            if char == 10:
                if self.dropping:
                    self._send("0 ERR LINE_TOO_LONG")
                elif self.line:
                    self._handle_line(bytes(self.line), now)
                self.line = bytearray()
                self.dropping = False
            elif not self.dropping:
                if len(self.line) < MAX_LINE:
                    self.line.append(char)
                else:
                    self.line = bytearray()
                    self.dropping = True

    def tick(self):
        now = self.clock.ticks_ms()
        if self.button_c.check() is True:
            self._latch("BUTTON_C")
        if (self.look_id is not None and
                self.clock.ticks_diff(now, self.last_keep) >= DRIVE_TIMEOUT_MS):
            self._stop()
            self._send("0 EVENT TIMEOUT")
        if (self.last_drive is not None and
                self.clock.ticks_diff(now, self.last_drive) >= DRIVE_TIMEOUT_MS):
            self._stop()
            self._send("0 EVENT TIMEOUT")
            print("UART CONTROL: drive timeout")
        if (self.ready and (self.last_edge_poll is None or
                self.clock.ticks_diff(now, self.last_edge_poll) >= EDGE_POLL_MS)):
            self.last_edge_poll = now
            readings = self._read_edge()
            if readings is not None and self._edge_seen(readings):
                self._latch("EDGE")
        self._poll_range(now)
        if self.safe_motion:
            if not self._range_valid(now):
                self._latch("SENSOR")
            elif self.left + self.right > 0 and self.distance_mm <= 150:
                self._stop()
                self._send("0 EVENT OBSTACLE")
        self._poll_uart(now)
        self._sample_calibration(now)
        if self.look_id is not None:
            left, right = self.voice_turn.update(now)
            if self.voice_turn.result is not None:
                result = self.voice_turn.result
                command_id, self.look_id = self.look_id, None
                self._stop()
                if result == "DONE":
                    self._send(command_id + " OK LOOK_DONE")
                else:
                    self._send(command_id + " ERR " + result)
                    self._latch("TURN_FAULT")
            else:
                self.motors.set_speeds(left, right)
                self.left, self.right = left, right
        pressed_a = self.button_a.check() is True
        pressed_b = self.button_b.check() is True
        if not (self.left or self.right) and self.look_id is None:
            if pressed_a:
                self._cancel_wake()
                self.face.play_sad()
            if pressed_b:
                self._cancel_wake()
                self.face.play_happy()
        # LCD transfers can take longer than a control-loop tick. Keep the
        # transport and motor safety checks ahead of display work.
        if not (self.left or self.right) and self.look_id is None:
            try:
                self.wake.update(now)
            except Exception as error:
                self._cancel_wake()
                print("UART CONTROL: wake animation error:", error)
                self._send("0 EVENT WAKE_FAILED")
            self.face.update()

    def close(self):
        self._stop()
        self.uart.deinit()
