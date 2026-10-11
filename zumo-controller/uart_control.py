"""Motor-safe, line-oriented UART control for Digit's Zumo 2040.

The XIAO sends ``<id> COMMAND [arguments]\n`` on UART0. This module has no
hardware imports so its timing and safety rules can be tested on a desktop.
"""


from digit_face_engine import EXPRESSIONS
from wake_reaction import WakeReaction
from reaction_catalog import CATALOG_VERSION
from voice_turn import VoiceTurn, WIGGLE_ACCELERATION, WIGGLE_DECELERATION, movement_target
from motion_jobs import MotionJobs, FRONT_STOP_MM
from circle_show import CircleShow


MAX_LINE = 159
MAX_DRIVE_SPEED = 6000
MAX_AUTO_SPEED = 3200
APPROACH_STOP_DISTANCES_MM = (50, 100)
SCENE_LABELS = ("NONE", "BATTERY", "MARKER", "SCREWDRIVER", "SEARCHING", "FOUND",
                "PERSON", "EXIT", "NOT_SURE", "LOST", "OBSTACLE", "CHASE", "READY", "STOPPED")
DEFAULT_ACCELERATION = 3000  # Motor speed units per second.
MIN_ACCELERATION = 500
MAX_ACCELERATION = 6000
MAX_RAMP_STEP_MS = 20  # Never catch up with a sudden jump after a slow loop.
DECELERATION = 12000  # Fixed braking rate, independent of the acceleration slider.
BRAKE_TIMEOUT_MS = 750
DRIVE_TIMEOUT_MS = 300
EDGE_POLL_MS = 20
EDGE_EXTRA_MARGIN = 60
EDGE_CONFIRM_MS = 20
EDGE_STRONG_READING = 950
CALIBRATION_INTERVAL_MS = 10
CALIBRATION_SAMPLES = 60
MAX_SURFACE_BASELINE = 850
MIN_EDGE_MARGIN = 180


class UARTController:
    def __init__(self, uart, motors, line_sensors, battery, face,
                 button_a, button_b, button_c, clock, rgb_leds=None,
                 gyro=None, turn_motor_sign=1, tof=None, speed_control=None):
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
        self.speed_control = speed_control
        self.raw_distance_mm = -1
        self.range_status = 255
        self.distance_mm = -1
        self.range_at = None
        self.last_range_poll = None
        self.range_failed = False
        self.safe_motion = False
        self.approach_stop_mm = 0
        self.approach_arrived = False
        self.wake = WakeReaction(face, rgb_leds, clock)
        self.dance = CircleShow(face, rgb_leds, clock)
        self.voice_turn = VoiceTurn(gyro, clock, turn_motor_sign) if gyro else None
        self.look_id = None
        self.last_keep = None
        self.look_acceleration = MAX_ACCELERATION
        self.wiggle_started = False
        self.wiggle_scene = None
        self.wiggle_display_started = None
        self.wiggle_leg = 0
        self.wiggle_face = "HAPPY"
        self.line = bytearray()
        self.dropping = False
        self.ready = False
        self.fault = "NONE"
        self.edge = False
        self.edge_since = [None] * 5
        self.thresholds = None
        self.blocked_thresholds = None
        self.left = 0
        self.right = 0
        self.target_left = self.target_right = 0
        self.track_speed_ratios = None
        self.ramp_left = self.ramp_right = 0.0
        self.acceleration = DEFAULT_ACCELERATION
        self.deceleration = DECELERATION
        self.ramp_at = clock.ticks_ms()
        self.braking = False
        self.brake_started = None
        self.last_drive = None
        self.last_edge_poll = None
        self.calibration = None
        self.motors.off()
        self.last_tick_at = None
        self.max_loop_gap_ms = 0
        self.loop_overruns = 0
        self.scene_wire = None
        self.scene_sequence = None
        self.wake_wire = None
        self.wake_sequence = None
        self.motion = MotionJobs(self)
        if hasattr(face, "enable_cooperative"):
            face.enable_cooperative()

    def _send(self, value):
        self.uart.write((value + "\n").encode())

    def _stop(self, smooth=False):
        self.track_speed_ratios = None
        if self.speed_control:
            self.speed_control.reset()
        smooth = smooth and self.ready and bool(self.left or self.right)
        already_braking = self.braking
        self.braking = smooth
        self.deceleration = DECELERATION
        if not (smooth and already_braking):
            self.brake_started = self.clock.ticks_ms() if smooth else None
            self.ramp_at = self.clock.ticks_ms()
        if not smooth:
            self.motors.off()
            self.left = 0
            self.right = 0
            self.ramp_left = self.ramp_right = 0.0
        self.target_left = self.target_right = 0
        self.last_drive = None
        if not smooth:
            self.safe_motion = False
            self.approach_stop_mm = 0
            self.approach_arrived = False
        if self.look_id is not None:
            self._send(self.look_id + " ERR LOOK_CANCELLED")
        self.look_id = None
        self.last_keep = None
        if self.voice_turn:
            self.voice_turn.cancel()
        if self.wiggle_started:
            self.face.play("neutral")
        self.wiggle_started = False
        self.wiggle_scene = None
        self.wiggle_display_started = None
        self._cancel_wake()
        if not smooth or self.motion.kind != "CIRCLE":
            self.dance.cancel()

    def _cancel_wake(self):
        self.wake_wire = self.wake_sequence = None
        try:
            self.wake.cancel()
        except Exception as error:
            print("UART CONTROL: wake restore error:", error)
            self._send("0 EVENT WAKE_FAILED")

    def _moving(self):
        return bool(self.left or self.right or self.target_left or self.target_right)

    def _target_motion(self, left, right, now, acceleration, deceleration=DECELERATION,
                       rate_ratios=None):
        if not self._moving():
            self.ramp_at = now
        self.target_left, self.target_right = movement_target(left), movement_target(right)
        self.track_speed_ratios = rate_ratios
        self.acceleration = acceleration
        self.deceleration = deceleration
        self.braking = False
        self.brake_started = None

    @staticmethod
    def _ramp_track(current, target, acceleration_step, deceleration_step):
        # Reverse only after braking to zero, using the fixed deceleration rate.
        if current * target < 0:
            target = 0
        step = acceleration_step if abs(target) > abs(current) else deceleration_step
        return min(target, current + step) if target > current else max(target, current - step)

    def _update_motion(self, now):
        elapsed = max(0, min(MAX_RAMP_STEP_MS, self.clock.ticks_diff(now, self.ramp_at)))
        self.ramp_at = now
        step = self.acceleration * elapsed / 1000.0
        braking_step = self.deceleration * elapsed / 1000.0
        targets = (self.target_left, self.target_right)
        if self.speed_control:
            try:
                if self.track_speed_ratios is None:
                    targets = self.speed_control.update(now, targets, (self.left, self.right), self.braking)
                else:
                    targets = self.speed_control.update(now, targets, (self.left, self.right),
                                                       self.braking, self.track_speed_ratios)
            except Exception as error:
                self._latch("ENCODER")
                print("ENCODER CONTROL:", error)
                return
        self.ramp_left = self._ramp_track(self.ramp_left, targets[0], step, braking_step)
        self.ramp_right = self._ramp_track(self.ramp_right, targets[1], step, braking_step)
        left, right = int(self.ramp_left), int(self.ramp_right)
        if (left, right) != (self.left, self.right):
            self.motors.set_speeds(left, right)
            self.left, self.right = left, right
        if self.braking and not (self.ramp_left or self.ramp_right):
            self.braking = False
            self.brake_started = None
            self.safe_motion = False

    def _latch(self, reason):
        if self.motion.armed or self.motion.active:
            self.motion.cancel(reason, True)
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
        now = self.clock.ticks_ms()
        confirmed = False
        for i in range(5):
            if readings[i] < self.thresholds[i]:
                self.edge_since[i] = None
            elif readings[i] >= max(EDGE_STRONG_READING, self.thresholds[i]):
                return True  # A strong edge indication still stops immediately.
            elif self.edge_since[i] is None:
                self.edge_since[i] = now
            elif self.clock.ticks_diff(now, self.edge_since[i]) >= EDGE_CONFIRM_MS:
                confirmed = True
        return confirmed

    def start_calibration(self, command_id="0", now=None):
        """Begin stationary surface sampling; ID 0 is used at startup."""
        if now is None:
            now = self.clock.ticks_ms()
        if self.calibration is not None:
            self._send(command_id + " ERR BUSY")
            return
        if self._moving():
            self._send(command_id + " ERR MOVING")
            return
        if self.button_c.is_pressed():
            self._send(command_id + " ERR BUTTON_C")
            return
        self._stop()
        self.ready = False
        self.edge_since = [None] * 5
        self.thresholds = None
        self.calibration = {
            "count": 0,
            "totals": [0] * 5,
            "next": now,
        }
        self._send(command_id + " OK CALIBRATING")
        print("UART CONTROL: calibrating on stationary surface")

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
            min(975, value + max(MIN_EDGE_MARGIN, (1024 - value) // 2) + EDGE_EXTRA_MARGIN)
            for value in baselines
        ]
        self.fault = "NONE"
        self.edge = False
        self.edge_since = [None] * 5
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
        age = self.clock.ticks_diff(self.clock.ticks_ms(), self.range_at) if self.range_at is not None else 65535
        velocity = self.speed_control.velocity if self.speed_control else (0, 0)
        target = self.speed_control.targets if self.speed_control else (0, 0)
        self._send("{} OK READY={} FAULT={} L={} R={} BAT={} EDGE={} VOICE={} DIST={} RANGE={} AUTO=1 BRAKE=1 LOOKRAMP=1 WIGGLE=2 SCENE=1 SAFE2=1 CLOSE=2 RA={} RS={} RAW={} ENC={} VL={} VR={} MOTION=1 BOOT={} SESSION={} GAP={} OVERRUN={} DSUS={} TL={} TR={} CIRCLE=2 SPIN=1 REACT={:08x} NR={} TRAVEL=1 DCAL={} EXPR=1 CL={} CR={}".format(
            command_id, int(self.ready), self.fault, self.left, self.right,
            millivolts, int(self.edge), int(self.voice_turn is not None),
            self.distance_mm, int(valid), min(65535, age), self.range_status,
            self.raw_distance_mm, int(self.speed_control is not None), int(velocity[0]), int(velocity[1]), self.motion.boot, self.motion.session,
            self.max_loop_gap_ms, self.loop_overruns, getattr(self.face, "max_slice_us", 0), int(target[0]), int(target[1]), CATALOG_VERSION, int(self._range_no_return(self.clock.ticks_ms())), int(self.motion.distance_scale is not None), *(getattr(self.speed_control,"counts",(0,0)) if self.speed_control else (0,0))))
        if self.motion.session:
            self.motion.snapshot(command_id)

    def _range_valid(self, now):
        return (self.tof is not None and self.range_at is not None and
                self.clock.ticks_diff(now, self.range_at) < 250 and
                self.distance_mm > 0)

    def _range_no_return(self, now):
        # ST status 2 is a fresh weak/no-return observation, not a measured
        # distance or a sensor outage. Never promote other error codes to it.
        return (self.tof is not None and self.range_at is not None and
                self.clock.ticks_diff(now, self.range_at) < 250 and
                not self.range_failed and self.range_status == 2)

    def _poll_range(self, now):
        if self.tof is None or (self.last_range_poll is not None and
                self.clock.ticks_diff(now, self.last_range_poll) < 20):
            return
        self.last_range_poll = now
        try:
            if self.tof.data_ready:
                if hasattr(self.tof, "read_result"):
                    distance, status = self.tof.read_result()
                else:
                    distance, status = self.tof.distance_mm, self.tof.range_status
                self.raw_distance_mm, self.range_status = distance, status
                valid = status == 0 and distance > 0
                self.tof.clear_interrupt()
                self.distance_mm = distance if valid else -1
                # Timestamp every completed observation, including no return.
                # distance_mm remains -1 unless the distance itself is valid.
                self.range_at = now
                self.range_failed = False
        except Exception as error:
            self.distance_mm = -1
            self.range_at = None
            if self.safe_motion:
                self._latch("SENSOR")
            if not self.range_failed:
                print("UART CONTROL: range sensor error:", error)
            self.range_failed = True

    def _apply_motion(self, command_id, left, right, now, reply,
                      acceleration=DEFAULT_ACCELERATION):
        if not self.ready:
            self._send(command_id + " ERR NOT_READY")
            return False
        if self.button_c.is_pressed():
            self._latch("BUTTON_C")
            self._send(command_id + " ERR BUTTON_C")
            return False
        readings = self._read_edge()
        if readings is None or self._edge_seen(readings):
            if readings is not None:
                self._latch("EDGE")
            self._send(command_id + " ERR " + self.fault)
            return False
        if self.look_id is not None:
            self._stop()  # Manual motion supersedes an automatic voice turn.
        else:
            self._cancel_wake()
        self.approach_stop_mm = 0
        self.approach_arrived = False
        if left or right:
            self._target_motion(left, right, now, acceleration)
            self.safe_motion = False
            self.last_drive = now
        else:
            self._stop(smooth=True)
        self._send(command_id + " OK " + reply)
        return True

    def _handle_line(self, raw, now):
        try:
            line = raw.decode("ascii")
        except UnicodeError:
            self._send("0 ERR FORMAT")
            return
        words = line.split()
        if len(line) > 63 and (len(words) < 2 or words[1] != "M2"):
            self._send("0 ERR LINE_TOO_LONG")
            return
        if len(words) < 2 or not words[0].isdigit():
            self._send("0 ERR FORMAT")
            return
        command_id = words[0]
        if not 1 <= int(command_id) <= 65535:
            self._send("0 ERR ID_RANGE")
            return
        command = words[1]
        args = words[2:]
        if command == "M2":
            self.motion.handle(command_id, line[len(command_id) + 1:], now)
            return
        if command in ("STOP", "BRAKE", "CALIBRATE", "DRIVE", "SAFE", "LOOK", "WIGGLE", "TURN") and self.motion.armed:
            # Manual takeover closes the autonomous session before accepting a
            # legacy command. Delayed autonomous traffic can never resume it.
            self.motion.cancel("CANCELLED" if command in ("STOP", "BRAKE") else "MANUAL_TAKEOVER", smooth=command == "BRAKE")
        if command == "PING" and not args:
            self._send(command_id + " OK PONG")
        elif command == "STATUS" and not args:
            self._status(command_id)
        elif command in ("STOP", "BRAKE") and not args:
            self.scene_wire = self.scene_sequence = None
            self._stop(smooth=command == "BRAKE")
            self.calibration = None
            self.face.play("neutral")
            self._send(command_id + (" OK BRAKING" if self.braking else " OK STOPPED"))
        elif command == "CALIBRATE" and not args:
            self.start_calibration(command_id, now)
        elif (command == "LOOK" and len(args) in (1, 2)) or (command == "WIGGLE" and (not args or args in (["HAPPY"], ["EXCITED"]))):
            argument = args[0] if command == "LOOK" else "15"
            if not (argument[1:] if argument.startswith("-") else argument).isdigit():
                self._send(command_id + " ERR ARGUMENT")
            elif len(args) == 2 and not args[1].isdigit():
                self._send(command_id + " ERR ARGUMENT")
            elif len(args) == 2 and not MIN_ACCELERATION <= int(args[1]) <= MAX_ACCELERATION:
                self._send(command_id + " ERR ACCELERATION_RANGE")
            elif not -180 <= int(argument) <= 180:
                self._send(command_id + " ERR ANGLE_RANGE")
            elif not self.ready:
                self._send(command_id + " ERR NOT_READY")
            elif self.button_c.is_pressed():
                self._latch("BUTTON_C")
                self._send(command_id + " ERR BUTTON_C")
            elif self.voice_turn is None:
                self._send(command_id + " ERR NO_GYRO")
            elif self.look_id is not None or self._moving() or self.calibration is not None:
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
                    if command == "WIGGLE":
                        self.look_acceleration = WIGGLE_ACCELERATION
                        self.wiggle_leg = 0
                        self.wiggle_face = args[0] if args else "HAPPY"
                        self.voice_turn.start_wiggle(now)
                        if getattr(self.face, "cooperative", False):
                            self.voice_turn.timeout_ms += 1500
                    else:
                        self.look_acceleration = int(args[1]) if len(args) == 2 else MAX_ACCELERATION
                        self.voice_turn.start(int(argument), now, self.look_acceleration)
                    self._send(command_id + " OK LOOKING")
        elif command == "KEEP" and len(args) == 1:
            # Only the original LOOK id can refresh its 300 ms lease.
            if self.look_id is not None and args[0] == self.look_id:
                self.last_keep = now
                self._send(command_id + " OK KEEP")
            else:
                self._send(command_id + " ERR NO_LOOK")
        elif (command == "DRIVE" and len(args) in (2, 3)) or (command == "SAFE" and len(args) in (2, 3, 4)):
            if not all((arg[1:] if arg.startswith("-") else arg).isdigit()
                       for arg in args):
                self._send(command_id + " ERR ARGUMENT")
                return
            left, right = int(args[0]), int(args[1])
            acceleration = int(args[2]) if len(args) >= 3 else DEFAULT_ACCELERATION
            close = command == "SAFE" and len(args) == 4
            stop_mm = int(args[3]) if close else 150
            if not (-MAX_DRIVE_SPEED <= left <= MAX_DRIVE_SPEED and
                    -MAX_DRIVE_SPEED <= right <= MAX_DRIVE_SPEED):
                self._send(command_id + " ERR SPEED_RANGE")
            elif not MIN_ACCELERATION <= acceleration <= MAX_ACCELERATION:
                self._send(command_id + " ERR ACCEL_RANGE")
            elif close and stop_mm not in APPROACH_STOP_DISTANCES_MM:
                self._send(command_id + " ERR DISTANCE_RANGE")
            elif command == "SAFE" and len(args) >= 3 and not (0 <= left <= MAX_AUTO_SPEED and 0 <= right <= MAX_AUTO_SPEED):
                self._send(command_id + " ERR AUTO_SPEED_RANGE")
            elif command == "SAFE" and not self._range_valid(now):
                self._stop()
                self._send(command_id + " ERR RANGE_UNAVAILABLE")
            elif command == "SAFE" and left + right > 0 and self.distance_mm < FRONT_STOP_MM:
                self._stop()
                self._send(command_id + " ERR OBSTACLE")
            elif close and (self.distance_mm <= stop_mm or self.approach_arrived):
                self._stop(smooth=True)
                self.approach_stop_mm = stop_mm
                self.approach_arrived = True
                self.safe_motion = self._moving()
                self._send(command_id + " OK ARRIVED")
            else:
                if self._apply_motion(command_id, left, right, now, command, acceleration):
                    self.approach_stop_mm = stop_mm if close else 0
                    if left or right:
                        self.safe_motion = command == "SAFE" and self._moving()
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
                self._apply_motion(command_id, speed, -speed, now, "TURN",
                                   MAX_ACCELERATION)
        elif command == "WAKE" and not args:
            if self._moving():
                self._send(command_id + " ERR MOVING")
            elif self.calibration is not None or self.wake.active or self.look_id is not None:
                self._send(command_id + " ERR BUSY")
            elif self.button_c.is_pressed():
                self._send(command_id + " ERR BUTTON_C")
            elif self.rgb_leds is None:
                self._send(command_id + " ERR LED_UNAVAILABLE")
            else:
                try:
                    self.scene_wire = self.scene_sequence = None
                    self.wake.start(now)
                except Exception as error:
                    self._cancel_wake()
                    print("UART CONTROL: wake reaction error:", error)
                    self._send(command_id + " ERR WAKE_IO")
                else:
                    self._send(command_id + " OK WAKE")
                    if getattr(self.face, "cooperative", False):
                        self.wake_wire = command_id
                        self.wake_sequence = self.wake.scene_sequence
        elif command == "SCENE" and len(args) in (2,3):
            expression, caption = args[:2]
            duration = int(args[2]) if len(args)==3 and args[2].isdigit() else None
            if len(args)==3 and (duration is None or not 500 <= duration <= 10000):
                self._send(command_id + " ERR ARGUMENT")
                return
            if expression != expression.upper() or expression.lower() not in EXPRESSIONS or caption not in SCENE_LABELS:
                self._send(command_id + " ERR ARGUMENT")
            elif (self._moving() or self.look_id is not None or self.calibration is not None) and not getattr(self.face, "cooperative", False):
                self._send(command_id + " ERR MOVING")
            else:
                self.scene_wire = self.scene_sequence = None
                try:
                    self._cancel_wake()
                    self.face.show_scene(expression.lower(), caption)
                    if duration is not None:
                        self.face.hold_ms=max(0,duration-getattr(self.face,"enter_ms",0)-getattr(self.face,"exit_ms",0))
                    self._send(command_id + " OK SCENE")
                    if getattr(self.face, "cooperative", False):
                        self.scene_wire = command_id
                        self.scene_sequence = self.face.scene_sequence
                except Exception as error:
                    self._send(command_id + " ERR DISPLAY")
                    print("UART CONTROL: scene error:", error)
        elif command == "FACE" and len(args) == 1:
            if (self._moving() or self.look_id is not None) and not getattr(self.face, "cooperative", False):
                self._send(command_id + " ERR MOVING")
            elif args[0] == args[0].upper() and args[0].lower() in EXPRESSIONS:
                self.scene_wire = self.scene_sequence = None
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
        elif command in ("PING", "STATUS", "STOP", "BRAKE", "CALIBRATE", "DRIVE", "SAFE", "TURN", "FACE", "SCENE", "LED", "WAKE", "LOOK", "WIGGLE", "KEEP"):
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
        if self.last_tick_at is not None:
            gap = max(0, self.clock.ticks_diff(now, self.last_tick_at))
            self.max_loop_gap_ms = max(self.max_loop_gap_ms, gap)
            if gap > 20:
                self.loop_overruns += 1
            if gap > 150 and self.motion.active:
                self.motion.cancel("CONTROL_STALE", True)
        self.last_tick_at = now
        if self.button_c.check() is True:
            self._latch("BUTTON_C")
        if (self.brake_started is not None and
                self.clock.ticks_diff(now, self.brake_started) >= BRAKE_TIMEOUT_MS):
            self._stop()
            self._send("0 EVENT TIMEOUT")
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
            elif self.left + self.right > 0 or self.target_left + self.target_right > 0:
                if self.distance_mm < FRONT_STOP_MM:
                    self._stop()
                    self._send("0 EVENT OBSTACLE")
                elif self.approach_stop_mm and not self.approach_arrived and self.distance_mm <= self.approach_stop_mm:
                    self._stop(smooth=True)
                    self.approach_arrived = True
                    self._send("0 EVENT ARRIVED")
        self._poll_uart(now)
        self._sample_calibration(now)
        self.motion.tick(now)
        if self.look_id is not None:
            left, right = self.voice_turn.update(now, max(abs(self.left), abs(self.right)))
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
                if self.voice_turn.wiggle and self.voice_turn.bias is not None:
                    if not self.wiggle_started:
                        cooperative = getattr(self.face, "cooperative", False)
                        if self.wiggle_scene is None:
                            # Accept one scene, then keep servicing the gyro,
                            # safety and bounded display work with tracks off.
                            try:
                                if self.wiggle_face == "EXCITED":
                                    self.face.show_scene("excited", "FOUND")
                                elif cooperative:
                                    self.face.show_scene("happy", "NONE")
                                else:
                                    self.face.show_happy()
                            except Exception as error:
                                self._send(self.look_id + " ERR LOOK_FACE")
                                self._latch("TURN_FAULT")
                                print("UART CONTROL: greeting face error:", error)
                                return
                            if cooperative:
                                self.wiggle_scene = self.face.scene_sequence
                                self.wiggle_display_started = now
                            else:
                                self.wiggle_started = True
                                self.voice_turn.last_sample = self.clock.ticks_ms()
                                self._send(self.look_id + " OK WIGGLE_START")
                                return  # Recheck safety after synchronous drawing.
                        if cooperative:
                            if self.face.visible_scene_sequence == self.wiggle_scene:
                                self.wiggle_started = True
                                self._send(self.look_id + " OK WIGGLE_START")
                            elif self.clock.ticks_diff(now, self.wiggle_display_started) >= 1500:
                                self._send(self.look_id + " ERR LOOK_FACE")
                                self._latch("TURN_FAULT")
                                return
                            # Also keep zero for the first visible tick: the
                            # next tick rechecks the lease and edge before drive.
                            left = right = 0
                    if self.voice_turn.wiggle_leg != self.wiggle_leg:
                        self.wiggle_leg = self.voice_turn.wiggle_leg
                        if self.wiggle_leg == 2:
                            self._send(self.look_id + " OK WIGGLE_CENTER")
                self._target_motion(left, right, now, self.look_acceleration,
                                    WIGGLE_DECELERATION if self.voice_turn.wiggle else DECELERATION)
        self._update_motion(now)
        pressed_a = self.button_a.check() is True
        pressed_b = self.button_b.check() is True
        if not self._moving() and self.look_id is None and not self.motion.active:
            if pressed_a:
                self._cancel_wake()
                self.face.play_sad()
            if pressed_b:
                self._cancel_wake()
                self.face.play_happy()
        if self.motion.kind == "CIRCLE" and self.motion.state in ("RUNNING", "BRAKING"):
            self.dance.update(now)
        # LCD transfers can take longer than a control-loop tick. Keep the
        # transport and motor safety checks ahead of display work.
        if getattr(self.face, "cooperative", False) or (not self._moving() and self.look_id is None and not self.motion.active):
            try:
                self.wake.update(now)
            except Exception as error:
                self._cancel_wake()
                print("UART CONTROL: wake animation error:", error)
                self._send("0 EVENT WAKE_FAILED")
            if getattr(self.face, "cooperative", False):
                self.face.update(max_units=8, max_us=3000)
            else:
                self.face.update()
        if self.scene_wire is not None:
            if getattr(self.face, "active_scene_sequence", self.scene_sequence) != self.scene_sequence:
                self.scene_wire = self.scene_sequence = None
            elif getattr(self.face, "visible_scene_sequence", -1) == self.scene_sequence:
                self._send(self.scene_wire + " OK SCENE_VISIBLE")
                self.scene_wire = self.scene_sequence = None
        if self.wake_wire is not None:
            if (not self.wake.active or
                    getattr(self.face, "active_scene_sequence", self.wake_sequence) != self.wake_sequence):
                self.wake_wire = self.wake_sequence = None
            elif getattr(self.face, "visible_scene_sequence", -1) == self.wake_sequence:
                self._send(self.wake_wire + " OK WAKE_VISIBLE")
                self.wake_wire = self.wake_sequence = None

    def close(self):
        self._stop()
        self.uart.deinit()
