from machine import I2C, Pin
from zumo_2040_robot import robot
from digit_display import Display
from zumo_2040_robot.vl53l4cd import VL53L4CD
import time


# Motor speeds range from -6000 to 6000.
DRIVE_SPEED = 2000
APPROACH_SPEED = 1200
RECOVERY_SPEED = 2000
TURN_MAX_SPEED = 2000

FRONT_WALL_MM = 100
SLOW_APPROACH_MM = 200
STEERING_GAIN = 100
MAX_STEERING_CORRECTION = 400

PROXIMITY_INTERVAL_MS = 25
PROXIMITY_FILTER_SAMPLES = 3
TOF_ADDRESS = 0x29
TOF_POLL_INTERVAL_MS = 10
TOF_DATA_TIMEOUT_MS = 500
DISPLAY_INTERVAL_MS = 100
DRIVE_LOG_INTERVAL_MS = 500
TURN_LOG_INTERVAL_MS = 100

TURN_ANGLE_DEG = 90
TURN_TOLERANCE_DEG = 3
TURN_SETTLE_MS = 200
TURN_TIMEOUT_MS = 3000
TURN_ENCODER_STALL_MS = 500
TURN_GYRO_TIMEOUT_MS = 200
TURN_KP = 350
TURN_KD = 7
MAX_BLOCKED_ORIENTATIONS = 3

GYRO_CALIBRATION_SAMPLES = 200
GYRO_CALIBRATION_TIMEOUT_MS = 5000

# Encoder targets for 100:1 motors. The encoders produce 12 counts per motor
# revolution, and one drive-sprocket revolution moves the track about 115.2 mm.
BACKUP_COUNTS = 529       # 50.8 mm, which is 2 inches
TURN_90_COUNTS = 687      # Edge-recovery fallback pivot
ENCODER_ACTION_TIMEOUT_MS = 3000

CALIBRATION_SAMPLES = 60
CALIBRATION_DELAY_MS = 10
MAX_SURFACE_BASELINE = 850
MIN_EDGE_MARGIN = 180

TURN_LEFT = -1
TURN_RIGHT = 1

STATE_CRUISE = 0
STATE_SLOW_APPROACH = 1
STATE_TURN_90 = 2
STATE_VERIFY_CLEAR = 3
STATE_EDGE_REVERSE = 4
STATE_EDGE_TURN = 5
STATE_FAILED = 6

STATE_NAMES = (
    "CRUISE",
    "SLOW APPROACH",
    "TURN 90",
    "VERIFY CLEAR",
    "EDGE REVERSE",
    "EDGE TURN",
    "FAILED",
)


def clamp(value, lower, upper):
    return max(lower, min(upper, value))


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def update_history(history, value):
    history.append(value)
    if len(history) > PROXIMITY_FILTER_SAMPLES:
        history.pop(0)
    return median(history)


def front_wall_seen(distance_mm, range_is_valid):
    return range_is_valid and distance_mm <= FRONT_WALL_MM


def forward_speed(distance_mm, range_is_valid):
    if range_is_valid and distance_mm <= FRONT_WALL_MM:
        return 0
    if range_is_valid and distance_mm <= SLOW_APPROACH_MM:
        return APPROACH_SPEED
    return DRIVE_SPEED


def proximity_strengths(left, front_left, front_right, right):
    return max(left, front_left), max(right, front_right)


def choose_turn_direction(left_strength, right_strength, fallback_direction):
    if left_strength > right_strength:
        return TURN_RIGHT
    if right_strength > left_strength:
        return TURN_LEFT
    return fallback_direction


def direction_name(direction):
    return "left" if direction == TURN_LEFT else "right"


def steering_speeds(base_speed, left_strength, right_strength):
    correction = clamp(
        (left_strength - right_strength) * STEERING_GAIN,
        -MAX_STEERING_CORRECTION,
        MAX_STEERING_CORRECTION,
    )
    left_speed = clamp(base_speed + correction, 0, 6000)
    right_speed = clamp(base_speed - correction, 0, 6000)
    return left_speed, right_speed, correction


def gyro_target_for_direction(direction):
    # The Zumo gyro example reports positive Z rotation for a left pivot.
    return TURN_ANGLE_DEG if direction == TURN_LEFT else -TURN_ANGLE_DEG


def gyro_turn_command(target_angle, measured_angle, turn_rate):
    command = (target_angle - measured_angle) * TURN_KP - turn_rate * TURN_KD
    return int(clamp(command, -TURN_MAX_SPEED, TURN_MAX_SPEED))


def show_message(display, lines):
    display.fill(0)
    line_height = 10
    start_y = max(0, (64 - len(lines) * line_height) // 2)
    for index, line in enumerate(lines):
        x = max(0, (128 - len(line) * 8) // 2)
        display.text(line, x, start_y + index * line_height, 1)
    display.show()


def show_status(
    display,
    state,
    tof_distance_mm,
    tof_valid,
    left_strength,
    right_strength,
    turn_direction,
):
    display.fill(0)
    display.text(STATE_NAMES[state], 0, 0, 1)
    tof_text = "{} mm".format(tof_distance_mm) if tof_valid else "---"
    display.text("TOF: {}".format(tof_text), 0, 14, 1)
    display.text("L:{} R:{}".format(left_strength, right_strength), 0, 28, 1)
    if state in (STATE_TURN_90, STATE_VERIFY_CLEAR):
        display.text("Turn: {}".format(direction_name(turn_direction)), 0, 42, 1)
    display.text("C: stop", 36, 54, 1)
    display.show()


def wait_for_a(button_a, button_c):
    while True:
        if button_c.check() is True:
            return False
        if button_a.check() is True:
            return True
        time.sleep_ms(10)


def calibrate_gyro(imu, button_c, display):
    show_message(display, ("GYRO CAL", "Keep Digit still", "C: cancel"))
    total = 0.0
    samples = 0
    started = time.ticks_ms()

    while samples < GYRO_CALIBRATION_SAMPLES:
        if button_c.check() is True:
            return None
        if time.ticks_diff(time.ticks_ms(), started) >= GYRO_CALIBRATION_TIMEOUT_MS:
            raise RuntimeError("Gyroscope calibration timed out")
        if imu.gyro.data_ready():
            imu.gyro.read()
            total += imu.gyro.last_reading_dps[2]
            samples += 1
        else:
            time.sleep_ms(1)

    bias = total / samples
    print("Gyroscope Z bias:", bias, "degrees/second")
    return bias


def calibrate_surface(line_sensors, button_c, display):
    show_message(display, ("Calibrating", "keep Digit", "still..."))
    totals = [0, 0, 0, 0, 0]
    minimums = [1024, 1024, 1024, 1024, 1024]
    maximums = [0, 0, 0, 0, 0]

    for _ in range(CALIBRATION_SAMPLES):
        if button_c.check() is True:
            return None, True
        readings = line_sensors.read()
        for index in range(5):
            reading = readings[index]
            totals[index] += reading
            minimums[index] = min(minimums[index], reading)
            maximums[index] = max(maximums[index], reading)
        time.sleep_ms(CALIBRATION_DELAY_MS)

    baselines = [total // CALIBRATION_SAMPLES for total in totals]
    thresholds = []
    for index in range(5):
        if baselines[index] >= MAX_SURFACE_BASELINE:
            print("Calibration rejected:", baselines)
            print("Minimums:", minimums, "Maximums:", maximums)
            return None, False
        remaining_range = 1024 - baselines[index]
        margin = max(MIN_EDGE_MARGIN, remaining_range // 2)
        thresholds.append(min(975, baselines[index] + margin))

    print("Surface baselines:", baselines)
    print("Edge thresholds:", thresholds)
    return thresholds, False


def edge_direction(readings, thresholds, fallback_direction):
    left_danger = max(0, readings[0] - thresholds[0])
    left_danger += max(0, readings[1] - thresholds[1])
    right_danger = max(0, readings[3] - thresholds[3])
    right_danger += max(0, readings[4] - thresholds[4])
    if left_danger > right_danger:
        return TURN_RIGHT
    if right_danger > left_danger:
        return TURN_LEFT
    return fallback_direction


def encoder_progress(encoders):
    left_count, right_count = encoders.get_counts()
    return abs(left_count), abs(right_count)


def continue_encoder_reverse(motors, left_count, right_count):
    left_speed = 0 if left_count >= BACKUP_COUNTS else -RECOVERY_SPEED
    right_speed = 0 if right_count >= BACKUP_COUNTS else -RECOVERY_SPEED
    motors.set_speeds(left_speed, right_speed)


def continue_encoder_turn(motors, direction, left_count, right_count):
    left_done = left_count >= TURN_90_COUNTS
    right_done = right_count >= TURN_90_COUNTS
    if direction == TURN_LEFT:
        left_speed = 0 if left_done else -RECOVERY_SPEED
        right_speed = 0 if right_done else RECOVERY_SPEED
    else:
        left_speed = 0 if left_done else RECOVERY_SPEED
        right_speed = 0 if right_done else -RECOVERY_SPEED
    motors.set_speeds(left_speed, right_speed)


motors = robot.Motors()
proximity_sensors = None
tof_sensor = None

try:
    motors.off()
    display = Display()
    button_a = robot.ButtonA()
    button_c = robot.ButtonC()
    proximity_sensors = robot.ProximitySensors()
    line_sensors = robot.LineSensors()
    encoders = robot.Encoders()

    try:
        i2c = I2C(id=0, scl=Pin(5), sda=Pin(4), freq=400_000)
        if TOF_ADDRESS not in i2c.scan():
            raise RuntimeError("VL53L4CD not found at I2C address 0x29")
        tof_sensor = VL53L4CD(i2c, TOF_ADDRESS)
        tof_sensor.start_ranging()

        imu = robot.IMU(i2c)
        if not imu.detect():
            raise RuntimeError("Zumo IMU was not detected")
        imu.reset()
        imu.enable_default()
        gyro_bias = calibrate_gyro(imu, button_c, display)
        if gyro_bias is None:
            print("Gyroscope calibration cancelled.")
            raise SystemExit
        print("VL53L4CD and IMU ready on I2C0.")
    except Exception as error:
        motors.off()
        print("Sensor startup error:", error)
        show_message(
            display,
            ("SENSOR ERROR", "Motors stopped", "Check console", "C: exit"),
        )
        while button_c.check() is not True:
            time.sleep_ms(10)
        raise

    show_message(display, ("Place Digit", "near center", "A: calibrate", "C: exit"))
    cancelled = not wait_for_a(button_a, button_c)
    edge_thresholds = None

    while edge_thresholds is None and not cancelled:
        edge_thresholds, cancelled = calibrate_surface(
            line_sensors, button_c, display
        )
        if edge_thresholds is None and not cancelled:
            show_message(
                display,
                ("Bad surface", "or movement", "A: retry", "C: exit"),
            )
            if not wait_for_a(button_a, button_c):
                cancelled = True

    if not cancelled:
        while True:
            show_message(display, ("Calibration OK", "A: start", "C: exit"))
            if not wait_for_a(button_a, button_c):
                cancelled = True
                break
            readings = line_sensors.read()
            if all(
                readings[index] < edge_thresholds[index]
                for index in range(5)
            ):
                break
            show_message(display, ("EDGE DETECTED", "Reposition", "and press A"))
            time.sleep_ms(500)

    if not cancelled:
        state = STATE_VERIFY_CLEAR
        state_started = time.ticks_ms()
        turn_direction = TURN_RIGHT
        edge_tie_direction = TURN_RIGHT
        obstacle_tie_direction = TURN_RIGHT
        blocked_orientations = 0
        verification_after_turn = False

        proximity_histories = [[], [], [], []]
        filtered_left = 0
        filtered_front_left = 0
        filtered_front_right = 0
        filtered_right = 0
        left_strength = 0
        right_strength = 0
        tof_distance_mm = 0
        tof_valid = False

        last_proximity_read = time.ticks_add(
            state_started, -PROXIMITY_INTERVAL_MS
        )
        last_tof_poll = time.ticks_add(state_started, -TOF_POLL_INTERVAL_MS)
        last_tof_measurement = state_started
        last_display_update = time.ticks_add(
            state_started, -DISPLAY_INTERVAL_MS
        )
        last_drive_log = time.ticks_add(state_started, -DRIVE_LOG_INTERVAL_MS)

        turn_target_angle = 0.0
        turn_angle = 0.0
        turn_rate = 0.0
        last_gyro_us = time.ticks_us()
        last_gyro_measurement = state_started
        turn_settle_started = None
        last_turn_encoder_progress = 0
        last_turn_encoder_activity = state_started
        last_turn_log = time.ticks_add(state_started, -TURN_LOG_INTERVAL_MS)

        motors.off()
        print("Box roaming armed. Waiting for a clear ToF reading.")

        while True:
            if button_c.check() is True:
                print("Button C pressed: stopping.")
                break

            now = time.ticks_ms()
            if state == STATE_FAILED:
                motors.off()
                time.sleep_ms(10)
                continue

            down_readings = line_sensors.read()
            edge_seen = any(
                down_readings[index] >= edge_thresholds[index]
                for index in range(5)
            )

            proximity_updated = False
            if time.ticks_diff(now, last_proximity_read) >= PROXIMITY_INTERVAL_MS:
                last_proximity_read = now
                try:
                    proximity_sensors.read()
                    raw_values = (
                        proximity_sensors.left_counts_with_left_leds(),
                        proximity_sensors.front_counts_with_left_leds(),
                        proximity_sensors.front_counts_with_right_leds(),
                        proximity_sensors.right_counts_with_right_leds(),
                    )
                    filtered_left = update_history(
                        proximity_histories[0], raw_values[0]
                    )
                    filtered_front_left = update_history(
                        proximity_histories[1], raw_values[1]
                    )
                    filtered_front_right = update_history(
                        proximity_histories[2], raw_values[2]
                    )
                    filtered_right = update_history(
                        proximity_histories[3], raw_values[3]
                    )
                    left_strength, right_strength = proximity_strengths(
                        filtered_left,
                        filtered_front_left,
                        filtered_front_right,
                        filtered_right,
                    )
                    proximity_updated = True
                except Exception as error:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("PROX ERROR", "Motors stopped", "C: exit"),
                    )
                    print("Proximity communication error:", error)
                    continue

            tof_updated = False
            if time.ticks_diff(now, last_tof_poll) >= TOF_POLL_INTERVAL_MS:
                last_tof_poll = now
                try:
                    if tof_sensor.data_ready:
                        measured_distance = tof_sensor.distance_mm
                        range_status = tof_sensor.range_status
                        tof_sensor.clear_interrupt()
                        last_tof_measurement = now
                        tof_valid = range_status == 0
                        tof_distance_mm = measured_distance if tof_valid else 0
                        tof_updated = True
                    elif (
                        time.ticks_diff(now, last_tof_measurement)
                        >= TOF_DATA_TIMEOUT_MS
                    ):
                        raise RuntimeError(
                            "VL53L4CD stopped producing measurements"
                        )
                except Exception as error:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("TOF ERROR", "Motors stopped", "C: exit"),
                    )
                    print("TOF communication error:", error)
                    continue

            gyro_updated = False
            if state == STATE_TURN_90:
                try:
                    if imu.gyro.data_ready():
                        imu.gyro.read()
                        raw_turn_rate = imu.gyro.last_reading_dps[2]
                        turn_rate = raw_turn_rate - gyro_bias
                        now_us = time.ticks_us()
                        elapsed_us = time.ticks_diff(now_us, last_gyro_us)
                        turn_angle += turn_rate * elapsed_us / 1000000
                        last_gyro_us = now_us
                        last_gyro_measurement = now
                        gyro_updated = True
                    elif (
                        time.ticks_diff(now, last_gyro_measurement)
                        >= TURN_GYRO_TIMEOUT_MS
                    ):
                        raise RuntimeError("Gyroscope stopped producing data")
                except Exception as error:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("GYRO ERROR", "Motors stopped", "C: exit"),
                    )
                    print("Gyroscope communication error:", error)
                    continue

            # Edge escape overrides every normal roaming maneuver.
            if state != STATE_EDGE_REVERSE and edge_seen:
                motors.off()
                turn_direction = edge_direction(
                    down_readings, edge_thresholds, edge_tie_direction
                )
                edge_tie_direction = -edge_tie_direction
                encoders.get_counts(reset=True)
                state = STATE_EDGE_REVERSE
                state_started = now
                motors.set_speeds(-RECOVERY_SPEED, -RECOVERY_SPEED)
                print("Edge detected:", list(down_readings))

            elif state == STATE_EDGE_REVERSE:
                elapsed = time.ticks_diff(now, state_started)
                left_count, right_count = encoder_progress(encoders)
                if left_count >= BACKUP_COUNTS and right_count >= BACKUP_COUNTS:
                    motors.off()
                    encoders.get_counts(reset=True)
                    if turn_direction == TURN_LEFT:
                        motors.set_speeds(-RECOVERY_SPEED, RECOVERY_SPEED)
                    else:
                        motors.set_speeds(RECOVERY_SPEED, -RECOVERY_SPEED)
                    state = STATE_EDGE_TURN
                    state_started = now
                    print("Two-inch edge retreat complete.")
                elif elapsed >= ENCODER_ACTION_TIMEOUT_MS:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("ENCODER STOP", "Reverse timeout", "C: exit"),
                    )
                    print("Failsafe stop: edge reverse timed out.")
                else:
                    continue_encoder_reverse(motors, left_count, right_count)

            elif state == STATE_EDGE_TURN:
                elapsed = time.ticks_diff(now, state_started)
                left_count, right_count = encoder_progress(encoders)
                if left_count >= TURN_90_COUNTS and right_count >= TURN_90_COUNTS:
                    motors.off()
                    state = STATE_VERIFY_CLEAR
                    state_started = now
                    verification_after_turn = False
                    blocked_orientations = 0
                    print("Edge turn complete; verifying front clearance.")
                elif elapsed >= ENCODER_ACTION_TIMEOUT_MS:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("ENCODER STOP", "Turn timeout", "C: exit"),
                    )
                    print("Failsafe stop: edge turn timed out.")
                else:
                    continue_encoder_turn(
                        motors, turn_direction, left_count, right_count
                    )

            elif state in (STATE_CRUISE, STATE_SLOW_APPROACH):
                if tof_updated and front_wall_seen(tof_distance_mm, tof_valid):
                    motors.off()
                    turn_direction = choose_turn_direction(
                        left_strength, right_strength, obstacle_tie_direction
                    )
                    if left_strength == right_strength:
                        obstacle_tie_direction = -obstacle_tie_direction
                    blocked_orientations = 0
                    verification_after_turn = False
                    turn_target_angle = gyro_target_for_direction(turn_direction)
                    turn_angle = 0.0
                    turn_rate = 0.0
                    last_gyro_us = time.ticks_us()
                    last_gyro_measurement = now
                    turn_settle_started = None
                    encoders.get_counts(reset=True)
                    last_turn_encoder_progress = 0
                    last_turn_encoder_activity = now
                    last_turn_log = time.ticks_add(now, -TURN_LOG_INTERVAL_MS)
                    state = STATE_TURN_90
                    state_started = now
                    print(
                        "Front wall:", tof_distance_mm,
                        "mm; L:", left_strength,
                        "R:", right_strength,
                        "turning", direction_name(turn_direction),
                    )
                else:
                    base_speed = forward_speed(tof_distance_mm, tof_valid)
                    new_state = (
                        STATE_SLOW_APPROACH
                        if base_speed == APPROACH_SPEED
                        else STATE_CRUISE
                    )
                    if state != new_state:
                        state = new_state
                        state_started = now
                        print("Drive state:", STATE_NAMES[state])
                    if proximity_updated or tof_updated:
                        left_motor, right_motor, correction = steering_speeds(
                            base_speed, left_strength, right_strength
                        )
                        motors.set_speeds(left_motor, right_motor)
                        if (
                            time.ticks_diff(now, last_drive_log)
                            >= DRIVE_LOG_INTERVAL_MS
                        ):
                            last_drive_log = now
                            print(
                                STATE_NAMES[state],
                                "TOF:", tof_distance_mm if tof_valid else "invalid",
                                "L:", left_strength,
                                "R:", right_strength,
                                "correction:", correction,
                                "motors:", left_motor, right_motor,
                            )

            elif state == STATE_TURN_90:
                elapsed = time.ticks_diff(now, state_started)
                error = turn_target_angle - turn_angle
                if elapsed >= TURN_TIMEOUT_MS:
                    motors.off()
                    state = STATE_FAILED
                    show_message(
                        display,
                        ("TURN ERROR", "Gyro timeout", "C: exit"),
                    )
                    print("Failsafe stop: gyro turn timed out at", turn_angle)
                elif abs(error) <= TURN_TOLERANCE_DEG:
                    motors.off()
                    if turn_settle_started is None:
                        turn_settle_started = now
                    elif time.ticks_diff(now, turn_settle_started) >= TURN_SETTLE_MS:
                        state = STATE_VERIFY_CLEAR
                        state_started = now
                        verification_after_turn = True
                        print(
                            "Gyro turn complete:", turn_angle,
                            "degrees; verifying clearance.",
                        )
                else:
                    turn_settle_started = None
                    left_count, right_count = encoder_progress(encoders)
                    progress = left_count + right_count
                    if progress > last_turn_encoder_progress:
                        last_turn_encoder_progress = progress
                        last_turn_encoder_activity = now
                    elif (
                        time.ticks_diff(now, last_turn_encoder_activity)
                        >= TURN_ENCODER_STALL_MS
                    ):
                        motors.off()
                        state = STATE_FAILED
                        show_message(
                            display,
                            ("TURN ERROR", "Encoder stall", "C: exit"),
                        )
                        print("Failsafe stop: no encoder movement during turn.")
                        continue
                    command = gyro_turn_command(
                        turn_target_angle, turn_angle, turn_rate
                    )
                    motors.set_speeds(-command, command)
                    if (
                        gyro_updated
                        and time.ticks_diff(now, last_turn_log)
                        >= TURN_LOG_INTERVAL_MS
                    ):
                        last_turn_log = now
                        print(
                            "Turning", direction_name(turn_direction),
                            "angle:", turn_angle,
                            "target:", turn_target_angle,
                        )

            elif state == STATE_VERIFY_CLEAR and tof_updated:
                if front_wall_seen(tof_distance_mm, tof_valid):
                    if verification_after_turn:
                        blocked_orientations += 1
                    else:
                        blocked_orientations = 0
                        turn_direction = choose_turn_direction(
                            left_strength, right_strength, obstacle_tie_direction
                        )
                        if left_strength == right_strength:
                            obstacle_tie_direction = -obstacle_tie_direction

                    if blocked_orientations >= MAX_BLOCKED_ORIENTATIONS:
                        motors.off()
                        state = STATE_FAILED
                        show_message(
                            display,
                            ("NO CLEAR PATH", "Motors stopped", "C: exit"),
                        )
                        print("Failsafe stop: three turned orientations blocked.")
                    else:
                        print(
                            "Direction blocked:", tof_distance_mm,
                            "mm; turning another 90 degrees",
                            direction_name(turn_direction),
                        )
                        turn_target_angle = gyro_target_for_direction(turn_direction)
                        turn_angle = 0.0
                        turn_rate = 0.0
                        last_gyro_us = time.ticks_us()
                        last_gyro_measurement = now
                        turn_settle_started = None
                        encoders.get_counts(reset=True)
                        last_turn_encoder_progress = 0
                        last_turn_encoder_activity = now
                        last_turn_log = time.ticks_add(
                            now, -TURN_LOG_INTERVAL_MS
                        )
                        state = STATE_TURN_90
                        state_started = now
                else:
                    verification_after_turn = False
                    blocked_orientations = 0
                    base_speed = forward_speed(tof_distance_mm, tof_valid)
                    state = (
                        STATE_SLOW_APPROACH
                        if base_speed == APPROACH_SPEED
                        else STATE_CRUISE
                    )
                    left_motor, right_motor, correction = steering_speeds(
                        base_speed, left_strength, right_strength
                    )
                    motors.set_speeds(left_motor, right_motor)
                    state_started = now
                    print(
                        "Front clear; driving", STATE_NAMES[state],
                        "TOF:", tof_distance_mm if tof_valid else "no target",
                        "with correction", correction,
                    )

            if (
                state != STATE_FAILED
                and time.ticks_diff(now, last_display_update)
                >= DISPLAY_INTERVAL_MS
            ):
                last_display_update = now
                show_status(
                    display,
                    state,
                    tof_distance_mm,
                    tof_valid,
                    left_strength,
                    right_strength,
                    turn_direction,
                )

            time.sleep_ms(5)

finally:
    motors.off()
    if tof_sensor is not None:
        try:
            tof_sensor.stop_ranging()
        except Exception as error:
            print("Could not stop VL53L4CD ranging:", error)
    if proximity_sensors is not None:
        proximity_sensors.ir_pulses.off()
    print("Motors stopped.")
