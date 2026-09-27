"""Check Digit's motor and encoder directions with the tracks lifted.

Run this file directly in Thonny after installing the two device-library
patches. Each motor runs forward briefly at a low speed, then stops.
"""

from zumo_2040_robot import robot
import time


TEST_SPEED = 1000
TEST_TIME_MS = 500

motors = robot.Motors()
encoders = robot.Encoders()

try:
    print("Lift both tracks clear of the table. Test starts in 3 seconds.")
    for seconds in (3, 2, 1):
        print(seconds)
        time.sleep(1)

    for side, left_speed, right_speed in (
        ("LEFT", TEST_SPEED, 0),
        ("RIGHT", 0, TEST_SPEED),
    ):
        encoders.get_counts(reset=True)
        print(side, "forward test")
        motors.set_speeds(left_speed, right_speed)
        time.sleep_ms(TEST_TIME_MS)
        motors.off()
        time.sleep_ms(100)
        left_count, right_count = encoders.get_counts()
        print("encoder counts: left =", left_count, "right =", right_count)
        print("The tested track should have moved forward and its count should be positive.")
        time.sleep(1)
finally:
    motors.off()
    print("Motors stopped.")
