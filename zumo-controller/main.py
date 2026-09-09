from digit_face import DigitFace
from zumo_2040_robot import robot
import time


face = DigitFace()
button_a = robot.ButtonA()
button_b = robot.ButtonB()

while True:
    if button_a.check() is True:
        face.play_sad()

    if button_b.check() is True:
        face.play_happy()

    face.update()
    time.sleep_ms(20)
