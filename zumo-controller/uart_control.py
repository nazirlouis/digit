"""Motor-safe, line-oriented UART control for Digit's Zumo 2040.

The XIAO sends ``<id> COMMAND [arguments]\n`` on UART0. This module has no
hardware imports so its timing and safety rules can be tested on a desktop.
"""


import math
from digit_face_engine import EXPRESSIONS
from wake_reaction import WakeReaction
from reaction_catalog import CATALOG_VERSION
from voice_turn import VoiceTurn, WIGGLE_ACCELERATION, WIGGLE_DECELERATION, movement_target
from motion_jobs import MotionJobs, FRONT_STOP_MM
from circle_show import CircleShow
from edge_retreat import EdgeRetreat
from edge_reverse import EdgeReverse
from expression_motion import ExpressionMotion, LINK_TIMEOUT_MS, MAX_LOOP_GAP_MS
from dance_motion import DanceMotion, DanceShow


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
EDGE_CLEAR_MS = 1000
EDGE_CLEAR_MARGIN = 60
EDGE_CLEAR_MAX_GAP_MS = 100
CALIBRATION_INTERVAL_MS = 10
CALIBRATION_SAMPLES = 60
MAX_SURFACE_BASELINE = 850
MIN_EDGE_MARGIN = 180


class UARTController:
    def __init__(self, uart, motors, line_sensors, battery, face,
                 button_a, button_b, button_c, clock, rgb_leds=None,
                 gyro=None, turn_motor_sign=1, tof=None, speed_control=None,
                 accelerometer=None):
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
        self.boot_health = None
        self.tof = tof
        self.speed_control = speed_control
        self.accelerometer = accelerometer
        self.orientation_at = self.orientation_since = None
        self.edge_release_mask = 0
        self.orientation_safe = False
        self.accel_at = self.accel_poll_at = None
        self.accel_reading = None
        self.accel_error = "NONE" if accelerometer is not None else "ACC_MISSING"
        self.raw_distance_mm = -1
        self.range_status = 255
        self.distance_mm = -1
        self.range_at = None
        self.last_range_poll = None
        self.range_failed = False
        self.range_good_at = None
        self.range_good_distance, self.range_good_status = -1, 255
        self.safe_motion = False
        self.approach_stop_mm = 0
        self.approach_arrived = False
        self.wake = WakeReaction(face, rgb_leds, clock)
        self.dance = CircleShow(face, rgb_leds, clock)
        self.dance_motion = DanceMotion(clock)
        self.dance_show = DanceShow(face, rgb_leds, clock)
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
        self.surface_thresholds = None  # Last successful calibration survives faults.
        self.blocked_thresholds = None
        self.edge_restore_thresholds = None
        self.edge_clear_since = self.edge_clear_sample_at = None
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
        self.last_line_readings = None
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
        self.edge_retreat = EdgeRetreat(self, accelerometer)
        self.edge_reverse = EdgeReverse(self)
        self.expression_motion = ExpressionMotion(clock)
        self.expression_wire = None
        self.expression_name = None
        self.expression_announced = False
        self.last_host_at = clock.ticks_ms()
        if hasattr(face, "enable_cooperative"):
            face.enable_cooperative()

    def _send(self, value):
        self.uart.write((value + "\n").encode())

    def _stop(self, smooth=False, expression_completed=False):
        self.edge_reverse.cancel()
        # Even a smooth dashboard Stop must immediately end these short pulses.
        if self.expression_motion.active:
            smooth = False
        self.expression_motion.cancel()
        dance_completed = self.dance_motion.result == "DANCE_DONE"
        self.dance_motion.cancel()
        self.dance_show.cancel(reset_face=not dance_completed)
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
        # Notify only after motor power is removed. Waiting/skipped gestures
        # never produce a START, so the speaker cannot invent a movement.
        if self.expression_announced:
            reason = "DONE" if expression_completed else "CANCEL"
            self._send(self.expression_wire + " EVENT GESTURE END " + self.expression_name + " " + reason)
        self.expression_wire = self.expression_name = None
        self.expression_announced = False
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

    def _queue_expression_motion(self, expression, now, require_link=True, wire=None):
        # A busy/locked request is face-only, never deferred until later idle.
        # Only bounded cooperative rendering may share a loop with track motion.
        if (not getattr(self.face, "cooperative", False) or
                not self.ready or self.fault != "NONE" or self.edge or
                self.thresholds is None or self.calibration is not None or
                self._moving() or self.braking or self.last_drive is not None or
                self.look_id is not None or self.motion.armed or self.motion.active or
                self.wake.active or self.button_c.is_pressed() or
                not (self._range_valid(now) or self._range_no_return(now))):
            return
        sequence = getattr(self.face, "active_scene_sequence", None)
        if sequence is not None:
            self.expression_motion.start(expression, sequence, now, require_link)
            self.expression_wire = wire
            self.expression_name = expression.upper()
            self.expression_announced = False

    def _update_expression_motion(self, now):
        gesture = self.expression_motion
        if (not self.ready or self.fault != "NONE" or self.calibration is not None or
                self.motion.armed or self.motion.active or self.look_id is not None or
                self.button_c.is_pressed() or
                not (self._range_valid(now) or self._range_no_return(now)) or
                (gesture.require_link and
                 self.clock.ticks_diff(now, self.last_host_at) >= LINK_TIMEOUT_MS)):
            self._stop()
            return
        left, right = gesture.update(now, self.face)
        if not gesture.active:
            self._stop(expression_completed=gesture.completed)
            return
        if (left != right and self._range_valid(now) and
                self.distance_mm < FRONT_STOP_MM):
            self._stop()
            return
        # Sample floor sensors before every new pulse, even between regular
        # safety polls. No expression can use the edge-retreat exception.
        if (left or right) and not (self.left or self.right):
            readings = self._read_edge()
            if readings is None:
                return
            if any(readings[i] >= self.thresholds[i] for i in range(5)):
                self._latch("EDGE")
                return
        if self.speed_control:
            try:
                # Keep encoder I/O/direction checks alive, but do not apply PI
                # power boosts to tiny gestures or ramp below breakaway effort.
                self.speed_control.update(now, (left, right), (self.left, self.right))
            except Exception as error:
                self._latch("ENCODER")
                print("EXPRESSION ENCODER:", error)
                return
        self.target_left, self.target_right = left, right
        self.ramp_left, self.ramp_right = float(left), float(right)
        self.ramp_at = now
        if (left, right) != (self.left, self.right):
            self.motors.set_speeds(left, right)
            self.left, self.right = left, right
        if (left or right) and self.expression_wire and not self.expression_announced:
            self.expression_announced = True
            self._send(self.expression_wire + " EVENT GESTURE START " + self.expression_name)

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
        if self.motion.kind == "DANCE" and self.motion.active:
            # Preserve encoder fault detection without PI boosts or a ramp
            # below measured breakaway effort during these short pulses.
            if self.speed_control:
                try:
                    self.speed_control.update(now, (self.target_left, self.target_right), (self.left, self.right))
                except Exception as error:
                    self.motion.cancel(str(error), True)
            return
        if self.expression_motion.active:
            self._update_expression_motion(now)
            return
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
                reason = str(error)
                if self.motion.active and reason in ("ENCODER_IO", "ENCODER_STALE", "ENCODER_STALL"):
                    self.motion.cancel(reason, True)
                    return
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
        # Only an ordinary edge stop may reuse its known-good calibration.
        # Sensor/encoder/button faults and failed automatic retreats stay locked.
        restore = self.thresholds[:] if reason == "EDGE" and self.thresholds is not None else None
        if restore is not None and not self.edge_retreat.active:
            self.edge_reverse.used = False
        self._stop()
        self.ready = False
        if reason == "EDGE" and self.thresholds is not None:
            self.blocked_thresholds = self.thresholds
        self.thresholds = None
        self.calibration = None
        self.fault = reason
        self.edge = reason.startswith("EDGE")
        self.edge_restore_thresholds = restore
        self.edge_clear_since = self.edge_clear_sample_at = None
        # Publish the reverse gate in the same checksummed terminal result.
        # The interrupted session is still closed and can never be replayed.
        if self.motion.armed or self.motion.active:
            self.motion.cancel(reason, True)
        self._send("0 EVENT " + reason)
        print("UART CONTROL: motor stop,", reason)

    def _read_edge(self):
        try:
            readings = self.line_sensors.read()
            if len(readings) != 5 or any(not isinstance(v, int) or not 0 <= v <= 65535 for v in readings):
                raise ValueError("expected five line readings")
            self.last_line_readings = readings[:]
            return readings
        except Exception as error:
            print("UART CONTROL: line sensor error:", error)
            self._latch("SENSOR")
            return None

    def _edge_seen(self, readings):
        now = self.clock.ticks_ms()
        confirmed = False
        for i in range(5):
            if self.edge_release_mask & (1 << i):
                if readings[i] < self.thresholds[i]:
                    self.edge_release_mask &= ~(1 << i)
                else:
                    continue  # Do not relatch the same edge after its completed reverse.
            if readings[i] < self.thresholds[i]:
                self.edge_since[i] = None
            elif readings[i] >= max(EDGE_STRONG_READING, self.thresholds[i]):
                return True  # A strong edge indication still stops immediately.
            elif self.edge_since[i] is None:
                self.edge_since[i] = now
            elif self.clock.ticks_diff(now, self.edge_since[i]) >= EDGE_CONFIRM_MS:
                confirmed = True
        return confirmed

    def _sample_edge_restore(self, now):
        # A requested reverse owns all restoration once it starts. Its final
        # encoder-confirmed rest must pass before readiness can return.
        # Cancelled reverse traffic must not regain readiness through this
        # ordinary pickup path, either.
        if self.edge_reverse.active or self.edge_reverse.used:
            return
        thresholds = self.edge_restore_thresholds
        if thresholds is None or self.fault != "EDGE" or self.calibration is not None:
            return
        previous = self.edge_clear_sample_at
        if previous is not None:
            gap = self.clock.ticks_diff(now, previous)
            if 0 <= gap < EDGE_POLL_MS:
                return
            if gap < 0 or gap > EDGE_CLEAR_MAX_GAP_MS:
                self.edge_clear_since = None
        self.edge_clear_sample_at = now
        readings = self._read_edge()
        if readings is None:
            return  # _read_edge latches SENSOR and revokes restoration.
        clear = (not self.button_c.is_pressed() and not self._moving() and
                 not self.braking and not self.motion.armed and not self.motion.active and
                 all(readings[i] < thresholds[i] - EDGE_CLEAR_MARGIN for i in range(5)))
        if not clear:
            self.edge_clear_since = None
        elif self.edge_clear_since is None:
            self.edge_clear_since = now
        elif self.clock.ticks_diff(now, self.edge_clear_since) >= EDGE_CLEAR_MS:
            self._restore_edge_ready(now)

    def _restore_edge_ready(self, now):
        # Called only after the owning recovery has verified its whole dwell.
        thresholds = self.edge_restore_thresholds
        if thresholds is None or self.fault != "EDGE" or self.calibration is not None:
            return False
        reverse_done = self.edge_reverse.active
        self._stop()
        if reverse_done:
            self.edge_reverse.state = "DONE"
        self.thresholds = thresholds
        self.blocked_thresholds = self.edge_restore_thresholds = None
        self.edge_clear_since = self.edge_clear_sample_at = None
        self.edge_since = [None] * 5
        self.ready, self.edge, self.fault = True, False, "NONE"
        if not reverse_done:
            self.edge_reverse.used = False
        self.last_edge_poll = now
        self._send("0 EVENT READY")
        print("UART CONTROL: surface restored; existing calibration retained, motors stopped")
        return True

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
        self.edge_reverse.used = False
        self.edge_release_mask = 0
        self.edge_restore_thresholds = None
        self.edge_clear_since = self.edge_clear_sample_at = None
        self.edge_since = [None] * 5
        self.thresholds = None
        self.calibration = {
            "count": 0,
            "totals": [0] * 5,
            "next": now,
            "user_requested": command_id != "0",
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
            min(975, value + max(MIN_EDGE_MARGIN, (1024 - value) * 3 // 4) + EDGE_EXTRA_MARGIN)
            for value in baselines
        ]
        self.surface_thresholds = self.thresholds[:]
        if self.fault == "ORIENTATION" and (not self.orientation_safe or self.orientation_at is None or
                self.clock.ticks_diff(now, self.orientation_at) > 100):
            self._latch("ORIENTATION")
            return
        if self.boot_health is not None and state["user_requested"]:
            try:
                self.boot_health.clear_after_user_calibration()
            except Exception:
                self._latch("BOOT_HEALTH")
                return
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
        self._send("{} OK READY={} FAULT={} L={} R={} BAT={} EDGE={} VOICE={} DIST={} RANGE={} AUTO=1 BRAKE=1 LOOKRAMP=1 WIGGLE=2 SCENE=1 SAFE2=1 CLOSE=2 RA={} RS={} RAW={} ENC={} VL={} VR={} MOTION=1 BOOT={} SESSION={} GAP={} OVERRUN={} DSUS={} TL={} TR={} CIRCLE=2 SPIN=1 REACT={:08x} NR={} TRAVEL=1 DCAL={} EXPR=1 CL={} CR={} ERET={} EREV=1 REV={} RECOVERY=1 PUSH=1 DANCE=1 E100=1 EDIRECT=1 EREC={}".format(
            command_id, int(self.ready), self.fault, self.left, self.right,
            millivolts, int(self.edge), int(self.voice_turn is not None),
            self.distance_mm, int(valid), min(65535, age), self.range_status,
            self.raw_distance_mm, int(self.speed_control is not None), int(velocity[0]), int(velocity[1]), self.motion.boot, self.motion.session,
            self.max_loop_gap_ms, self.loop_overruns, getattr(self.face, "max_slice_us", 0), int(target[0]), int(target[1]), CATALOG_VERSION, int(self._range_no_return(self.clock.ticks_ms())), int(self.motion.distance_scale is not None), *(getattr(self.speed_control,"counts",(0,0)) if self.speed_control else (0,0)), int(self.speed_control is not None and self.motion.distance_scale is not None), int(self.edge_reverse.available() or self.edge_reverse.active), int(self.edge_reverse.active and self.edge_reverse.automatic)))
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
                if valid or status == 2:
                    self.range_good_at = now
                    self.range_good_distance, self.range_good_status = self.distance_mm, status
        except Exception as error:
            self.distance_mm = -1
            self.range_at = None
            if self.safe_motion:
                self._latch("SENSOR")
            if not self.range_failed:
                print("UART CONTROL: range sensor error:", error)
            self.range_failed = True

    def _sample_accelerometer(self, now):
        # One hardware reader; consumers share the ORIGINAL sample timestamp.
        # Reading the sensor clears data-ready, so independent readers starve.
        if self.accel_poll_at == now:
            return
        self.accel_poll_at = now
        acc = self.accelerometer
        if acc is None:
            self.accel_reading = self.accel_at = None
            self.accel_error = "ACC_MISSING"
            return
        try:
            if not acc.data_ready():
                return
            acc.read()
            values = tuple(acc.last_reading_g)
            if len(values) != 3 or not all(math.isfinite(v) for v in values):
                self.accel_reading = self.accel_at = None
                self.accel_error = "ACC_INVALID"
                return
            self.accel_reading, self.accel_at = values, now
            self.accel_error = "NONE"
        except Exception:
            self.accel_reading = self.accel_at = None
            self.accel_error = "ACC_IO"

    def _poll_orientation(self, now):
        if ((self.edge_retreat.active and not self.edge_retreat.blocked) or
                self.edge_reverse.active or self.edge_reverse.available()):
            self.orientation_since = None
            return  # Edge-triggered 100 mm reverse explicitly ignores IMU gates.
        # Only persistent, near-gravity sideways/inverted observations count.
        # A forward launch/contact impulse changes acceleration magnitude and
        # must not masquerade as gravity-based tilt. No pose is inferred from
        # absent/stale accelerometer data.
        self._sample_accelerometer(now)
        try:
            if self.accel_at is None or self.accel_at == self.orientation_at:
                if self.accel_error != "NONE" or self.orientation_at is None or self.clock.ticks_diff(now, self.orientation_at) > 100:
                    self.orientation_since = None
                return
            x, y, z = self.accel_reading
            norm = x*x + y*y + z*z
            if not all(math.isfinite(v) for v in (x, y, z)):
                self.orientation_since = None
                return
            if self.orientation_at is not None and self.clock.ticks_diff(now, self.orientation_at) > 100:
                self.orientation_since = None
            self.orientation_at = self.accel_at
            self.orientation_safe = .70 <= norm <= 1.30 and z >= .5 * math.sqrt(norm)
            # More than 75 degrees from upright for 200 ms is extreme tilt.
            severe = .70 <= norm <= 1.30 and z < .258819 * math.sqrt(norm)
            if not severe:
                self.orientation_since = None
            elif self.orientation_since is None:
                self.orientation_since = now
            elif self.clock.ticks_diff(now, self.orientation_since) >= 200 and (self.fault != "ORIENTATION" or self.calibration is not None):
                self._latch("ORIENTATION")
        except Exception:
            self.orientation_since = None

    def _apply_motion(self, command_id, left, right, now, reply,
                      acceleration=DEFAULT_ACCELERATION):
        if self.edge_reverse.active and self.edge_reverse.automatic:
            self._send(command_id + " OK STOPPED")
            return False  # Stale held DRIVE traffic cannot cancel/steer recovery.
        # Held recovery traffic cannot become ordinary reverse on clear floor.
        if self.edge_reverse.manual and self.edge_reverse.state == "DONE":
            self._send(command_id + " OK STOPPED")
            return False
        if not self.ready and self.fault == "EDGE" and reply == "DRIVE":
            if left == right and left < 0:
                if self.edge_reverse.active or self.edge_reverse.begin(now, manual=True):
                    self.last_drive = now
                    self._send(command_id + " OK DRIVE")
                    return True
            self._stop()
            self._send(command_id + " ERR " + ("EDGE_REVERSE_ONLY" if self.edge_reverse.available() else "NOT_READY"))
            return False
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
                if not self.edge_reverse.begin_automatic(readings, now) and self.fault in ("NONE", "EDGE"):
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
        if command in ("STOP", "BRAKE", "CALIBRATE", "DRIVE", "SAFE", "LOOK", "WIGGLE", "TURN", "WAKE") and self.expression_motion.active:
            self._stop()  # Manual actions take priority, including during pauses.
        if command in ("STOP", "BRAKE", "CALIBRATE", "DRIVE", "SAFE", "LOOK", "WIGGLE", "TURN") and self.motion.armed:
            # Manual takeover closes the autonomous session before accepting a
            # legacy command. Delayed autonomous traffic can never resume it.
            self.motion.cancel("CANCELLED" if command in ("STOP", "BRAKE") else "MANUAL_TAKEOVER", smooth=command == "BRAKE")
        if command == "PING" and not args:
            self.last_host_at = now
            self._send(command_id + " OK PONG")
        elif command == "STATUS" and not args:
            self.last_host_at = now
            self._status(command_id)
        elif command in ("STOP", "BRAKE") and not args:
            self.edge_reverse.manual = False
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
                if self.expression_motion.active:
                    self._stop()
                self.scene_wire = self.scene_sequence = None
                try:
                    self._cancel_wake()
                    self.face.show_scene(expression.lower(), caption)
                    if duration is not None:
                        self.face.hold_ms=max(0,duration-getattr(self.face,"enter_ms",0)-getattr(self.face,"exit_ms",0))
                    self.last_host_at = now
                    self._queue_expression_motion(expression.lower(), now, wire=command_id)
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
                if self.expression_motion.active:
                    self._stop()
                self.scene_wire = self.scene_sequence = None
                self._cancel_wake()
                if getattr(self.face, "cooperative", False):
                    self.face.show_scene(args[0].lower(), "NONE")
                    self.last_host_at = now
                    self._queue_expression_motion(args[0].lower(), now, wire=command_id)
                else:
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
            if gap > MAX_LOOP_GAP_MS:
                if self.motion.kind == "DANCE" and self.motion.active:
                    self.motion.cancel("DANCE_CONTROL_STALE", True)
                elif self.expression_motion.active:
                    self._stop()
        self.last_tick_at = now
        if self.button_c.check() is True:
            self._latch("BUTTON_C")
        if (self.brake_started is not None and
                self.clock.ticks_diff(now, self.brake_started) >= BRAKE_TIMEOUT_MS):
            if self.motion.active:
                self.motion.cancel("BRAKE_TIMEOUT", True)
            else:
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
        if ((self.ready or (self.edge_retreat.active and self.edge_retreat.blocked)) and (self.last_edge_poll is None or
                self.clock.ticks_diff(now, self.last_edge_poll) >= EDGE_POLL_MS)):
            self.last_edge_poll = now
            readings = self._read_edge()
            if readings is not None:
                self.edge_retreat.line_sample(readings, now)
                if self.ready and self._edge_seen(readings):
                    recovered = (self.edge_retreat.begin(readings, now) if self.motion.armed and self.motion.active
                                 else self.edge_reverse.begin_automatic(readings, now))
                    if not recovered and self.fault in ("NONE", "EDGE"):
                        self._latch("EDGE")
        self._poll_orientation(now)
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
        # Process queued Stop/calibration requests before considering readiness.
        # This never arms a motion session or replays an interrupted command.
        self._sample_edge_restore(now)
        self.motion.tick(now)
        if self.edge_reverse.active and self.motion.kind != "BACK":
            lease = self.last_drive
            self.edge_reverse.tick(now)
            if self.edge_reverse.active:
                self.last_drive = lease
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
                if self.expression_motion.active:
                    self._stop()
                self._cancel_wake()
                if getattr(self.face, "cooperative", False):
                    self.face.show_scene("sad", "NONE")
                    self._queue_expression_motion("sad", now, require_link=False)
                else:
                    self.face.play_sad()
            if pressed_b:
                if self.expression_motion.active:
                    self._stop()
                self._cancel_wake()
                if getattr(self.face, "cooperative", False):
                    self.face.show_scene("happy", "NONE")
                    self._queue_expression_motion("happy", now, require_link=False)
                else:
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
