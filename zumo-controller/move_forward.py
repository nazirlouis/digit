from zumo_2040_robot import robot
from digit_face import DigitFace
import time


MOTOR_SPEED = 2000
DRIVE_TIME_MS = 5000
STARTUP_GRACE_MS = 500
SUDDEN_DROP_G = 0.35

motors = robot.Motors()
face = DigitFace()
imu = robot.IMU()
imu.reset()
imu.enable_default()

# Read acceleration more frequently so a short impact is less likely to be
# missed.  The 4 g range also gives the sensor room to measure a hard bump.
imu.acc.set_output_data_rate(208)
imu.acc.set_full_scale(4)

# Measure the resting X-axis acceleration before the motors start.  X is the
# Zumo's forward/backward axis.
resting_ax = 0
calibration_samples = 40
for _ in range(calibration_samples):
    imu.acc.read()
    resting_ax += imu.acc.last_reading_g[0]
    face.update()
    time.sleep_ms(5)

last_ax = resting_ax / calibration_samples
collision_detected = False

try:
    # Both motors use the same positive speed to drive straight forward.
    motors.set_speeds(MOTOR_SPEED, MOTOR_SPEED)

    start_time = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), start_time) < DRIVE_TIME_MS:
        now = time.ticks_ms()

        if imu.acc.data_ready():
            imu.acc.read()
            ax = imu.acc.last_reading_g[0]

            # Ignore the motor's normal startup motion.  After that, a large
            # one-reading decrease indicates a sudden stop or frontal impact.
            if time.ticks_diff(now, start_time) >= STARTUP_GRACE_MS:
                acceleration_drop = last_ax - ax
                if acceleration_drop >= SUDDEN_DROP_G:
                    collision_detected = True
                    motors.set_speeds(0, 0)
                    print("Sudden acceleration drop:", acceleration_drop, "g")
                    break

            last_ax = ax

        # Keep Digit's eyes blinking and looking around while moving.
        face.update()
        time.sleep_ms(5)

    if collision_detected:
        # Hold a surprised expression until the script is stopped.
        face.play("surprised", hold_ms=-1)
        while True:
            face.update()
            time.sleep_ms(20)
finally:
    # Always stop, including if the program is interrupted in Thonny.
    motors.set_speeds(0, 0)
