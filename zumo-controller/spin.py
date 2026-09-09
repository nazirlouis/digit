# This program spins the robot in place while Digit shows a dizzy face.
# Keep the robot on the floor, guard it closely, and disconnect USB first.

from digit_face import DigitFace
from zumo_2040_robot import robot
import time


def wait_with_face(face, duration_ms):
    end_time = time.ticks_add(time.ticks_ms(), duration_ms)
    while time.ticks_diff(end_time, time.ticks_ms()) > 0:
        face.update()
        time.sleep_ms(10)


face = DigitFace()
rgb_leds = robot.RGBLEDs()
motors = robot.Motors()
buzzer = robot.Buzzer()

circus = (
    "! O6 L8 T180"
    "MS aa- L16 ML ga-gg- L8 MS fe ML e- MS e fe L16 ML e-ee-d L8 MS d-c ML <b MS c"
    "MS e L16 <b<b L8 ML <b-<b"
    "MS e L16 <b<b L8 ML <b-<b"
    "L16 <a-<a<b-<bcd-de- L8 MS fe ML e- MS e"
)

rgb_leds.set(0, [255, 0, 0])
rgb_leds.set(1, [192, 64, 0])
rgb_leds.set(2, [128, 128, 0])
rgb_leds.set(3, [0, 255, 0])
rgb_leds.set(4, [0, 0, 255])
rgb_leds.set(5, [128, 0, 128])
rgb_leds.show()

face.show_happy()
time.sleep_ms(1200)

face.start_dizzy()
buzzer.play_in_background(circus)

max_speed = motors.MAX_SPEED
speed_step = max_speed // 100

try:
    for speed in range(0, max_speed, speed_step):
        motors.set_speeds(speed, -speed)
        face.update()
        time.sleep_ms(10)

    wait_with_face(face, 500)

    for speed in range(max_speed, 0, -speed_step):
        motors.set_speeds(speed, -speed)
        face.update()
        time.sleep_ms(15)

    for speed in range(0, -max_speed, -speed_step):
        motors.set_speeds(speed, -speed)
        face.update()
        time.sleep_ms(15)

    wait_with_face(face, 500)

    for speed in range(-max_speed, 0, speed_step):
        motors.set_speeds(speed, -speed)
        face.update()
        time.sleep_ms(10)

finally:
    motors.off()
    rgb_leds.off()
    buzzer.off()
    face.stop_dizzy()
