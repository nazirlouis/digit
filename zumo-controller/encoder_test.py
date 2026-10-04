# Digit test: uses the LCD display driver, including direct Thonny runs.
# Install the silent buzzer replacement first; LCD CS uses GP7.
# Displays encoder counts on the screen and blinks the yellow LED with
# each tick.

from digit_display import Display
from zumo_2040_robot import robot

led = robot.YellowLED()
encoders = robot.Encoders()
display = Display()
buttonA = robot.ButtonA()
buttonB = robot.ButtonB()

while True:
    c = encoders.get_counts()

    # change LED on every count
    led((c[0] + c[1]) % 2)

    display.fill_rect(0, 0, 128, 18, 0)
    display.text("Left: "+str(c[0]), 0, 0)
    display.text("Right: "+str(c[1]), 0, 10)
    display.text("Press A to reset", 0, 30)
    display.text("Press B to exit", 0, 40)
    display.show()

    if buttonA.check():
        encoders.get_counts(reset = True)
    if buttonB.check():
        break
