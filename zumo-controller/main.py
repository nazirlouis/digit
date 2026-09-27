"""Digit's default face and UART-controlled manual drive program."""

from machine import Pin, UART
from digit_face import DigitFace
from uart_control import UARTController
from zumo_2040_robot import robot
import time


motors = robot.Motors()
motors.off()
uart = None
controller = None

try:
    face = DigitFace()
    uart = UART(0, baudrate=115200, bits=8, parity=None, stop=1,
                tx=Pin(28), rx=Pin(29), timeout=0, rxbuf=512)
    controller = UARTController(
        uart, motors, robot.LineSensors(), robot.Battery(), face,
        robot.ButtonA(), robot.ButtonB(), robot.ButtonC(), time,
    )
    print("UART CONTROL: GP28 TX, GP29 RX, 115200 8N1")
    print("UART CONTROL: motors stopped; send CALIBRATE on safe floor")
    while True:
        controller.tick()
        time.sleep_ms(5)
finally:
    motors.off()
    if controller is not None:
        controller.close()
    elif uart is not None:
        uart.deinit()
    print("UART CONTROL: motors stopped")
