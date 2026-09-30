"""Run directly in Thonny to try every face; motors are kept stopped."""

import time
from digit_face import DigitFace
from zumo_2040_robot import robot


def run():
    motors = robot.Motors()
    motors.off()
    face = DigitFace()
    button_a, button_b, button_c = robot.ButtonA(), robot.ButtonB(), robot.ButtonC()
    selected = 0
    print("FACE TEST: motors stopped")
    print("FACE TEST: A=next expression, B=replay, C=exit")
    print("FACE TEST: sleeping stays asleep; A selects the next face")
    print("FACE TEST:", ", ".join(face.EXPRESSIONS))
    print("FACE TEST: 1/{} neutral".format(len(face.EXPRESSIONS)))
    try:
        while button_c.check() is not True:
            advance = button_a.check() is True
            replay = button_b.check() is True
            if advance or replay:
                if advance:
                    selected = (selected + 1) % len(face.EXPRESSIONS)
                name = face.EXPRESSIONS[selected]
                print("FACE TEST: {}/{} {}".format(selected + 1, len(face.EXPRESSIONS), name))
                face.play(name)
            face.update()
            time.sleep_ms(5)
    finally:
        motors.off()
        face.display.fill(0)
        face.display.show()
        print("FACE TEST: stopped; motors off")


if __name__ == "__main__":
    run()
