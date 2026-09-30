"""Digit's default face and UART-controlled manual drive program."""

from machine import I2C, Pin, UART
import digit_config
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
    rgb_leds = robot.RGBLEDs()
    rgb_leds.set_brightness(5)
    gyro = None
    try:
        imu = robot.IMU(I2C(0, scl=Pin(5), sda=Pin(4), freq=400_000))
        if not imu.detect():
            raise RuntimeError("IMU not detected")
        imu.reset()
        imu.enable_default()
        gyro = imu.gyro
        print("VOICE TURN: gyro available; calibrated before each turn")
    except Exception as error:
        print("VOICE TURN: unavailable:", error)
    uart = UART(0, baudrate=115200, bits=8, parity=None, stop=1,
                tx=Pin(28), rx=Pin(29), timeout=0, rxbuf=512)
    controller = UARTController(
        uart, motors, robot.LineSensors(), robot.Battery(), face,
        robot.ButtonA(), robot.ButtonB(), robot.ButtonC(), time, rgb_leds,
        gyro, getattr(digit_config, "VOICE_TURN_MOTOR_SIGN", 1),
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
