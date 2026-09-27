"""Run in Thonny with both Zumo tracks lifted clear of the floor.

Each short pulse tests one track or both tracks, then stops. The shell reports
encoder movement and battery voltage so a weak supply can be distinguished
from one motor failing to start in a particular direction.
"""

from zumo_2040_robot import robot
import time


PULSE_MS = 500
SAMPLE_MS = 50
PAUSE_MS = 350

motors = robot.Motors()
encoders = robot.Encoders()
battery = robot.Battery()
motors.off()


def pulse(label, left, right):
    encoders.get_counts(reset=True)
    before = battery.get_level_millivolts()
    minimum = before
    motors.set_speeds(left, right)
    try:
        for _ in range(PULSE_MS // SAMPLE_MS):
            time.sleep_ms(SAMPLE_MS)
            voltage = battery.get_level_millivolts()
            if voltage < minimum:
                minimum = voltage
        left_count, right_count = encoders.get_counts()
    finally:
        motors.off()
    print("{}: command=({}, {}), counts=({}, {}), battery={} mV, min={} mV, drop={} mV".format(
        label, left, right, left_count, right_count,
        before, minimum, before - minimum))
    time.sleep_ms(PAUSE_MS)


try:
    print("Lift BOTH tracks clear of the table. Keep hands clear of tracks.")
    print("The test starts in 3 seconds; press Ctrl+C to abort.")
    for seconds in (3, 2, 1):
        print(seconds)
        time.sleep(1)
    for speed in (4000, 5000, 6000):
        pulse("LEFT forward {}".format(speed), speed, 0)
        pulse("RIGHT forward {}".format(speed), 0, speed)
        pulse("LEFT reverse {}".format(speed), -speed, 0)
        pulse("RIGHT reverse {}".format(speed), 0, -speed)
        pulse("BOTH forward {}".format(speed), speed, speed)
        pulse("BOTH reverse {}".format(speed), -speed, -speed)
finally:
    motors.off()
    print("Motors stopped.")
