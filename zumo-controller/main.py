"""Digit's default face and UART-controlled manual drive program."""

from machine import I2C, Pin, UART, WDT, mem32
import digit_config
from digit_face import DigitFace
from uart_control import UARTController
from zumo_2040_robot import robot
import time


motors = robot.Motors()
motors.off()
uart = None
controller = None
watchdog = None

try:
    face = DigitFace()
    rgb_leds = robot.RGBLEDs()
    rgb_leds.set_brightness(5)
    gyro = None
    sensor_bus = I2C(0, scl=Pin(5), sda=Pin(4), freq=400_000)
    try:
        imu = robot.IMU(sensor_bus)
        if not imu.detect():
            raise RuntimeError("IMU not detected")
        imu.reset()
        imu.enable_default()
        gyro = imu.gyro
        print("VOICE TURN: gyro available; calibrated before each turn")
    except Exception as error:
        print("VOICE TURN: unavailable:", error)
    tof = None
    try:
        from zumo_2040_robot.vl53l4cd import VL53L4CD
        tof = VL53L4CD(sensor_bus, 0x29)
        tof.start_ranging()
        print("AUTOMATIC DRIVE: front distance sensor available")
    except Exception as error:
        tof = None
        print("AUTOMATIC DRIVE: distance sensor unavailable:", error)
    uart = UART(0, baudrate=115200, bits=8, parity=None, stop=1,
                tx=Pin(28), rx=Pin(29), timeout=0, rxbuf=1024, txbuf=1024)
    speed_control = None
    import json
    from encoder_speed import EncoderSpeed
    try:
        with open("encoder_calibration.json") as calibration_file:
            speed_calibration = json.load(calibration_file)
    except OSError as error:
        if error.args[0] != 2:
            raise
        speed_calibration = None
        print("ENCODER CONTROL: calibration missing; existing motor control")
    if speed_calibration is not None:
        # An encoder/calibration failure must not silently select open-loop drive.
        speed_control = EncoderSpeed(robot.Encoders(), time, speed_calibration)
        print("ENCODER CONTROL: calibrated velocity feedback enabled")
    controller = UARTController(
        uart, motors, robot.LineSensors(), robot.Battery(), face,
        robot.ButtonA(), robot.ButtonB(), robot.ButtonC(), time, rgb_leds,
        gyro, getattr(digit_config, "VOICE_TURN_MOTOR_SIGN", 1), tof, speed_control,
    )
    print("UART CONTROL: GP28 TX, GP29 RX, 115200 8N1")
    print("UART CONTROL: motors stopped; startup surface calibration")
    controller.start_calibration()
    # RP2040's hardware watchdog catches an interpreter/I2C/display wedge.
    # It is fed only after a complete controller tick; reset always enters the
    # motors.off boot path and creates a new motion protocol boot identity.
    watchdog = WDT(timeout=1000)
    while True:
        tick_started = time.ticks_ms()
        controller.tick()
        watchdog.feed()
        remaining = 5 - time.ticks_diff(time.ticks_ms(), tick_started)
        if remaining > 0:
            time.sleep_ms(remaining)
finally:
    motors.off()
    if watchdog is not None:
        # RP2040 WATCHDOG_CTRL atomic CLR alias: disable ENABLE (bit 30)
        # only AFTER motors.off, when exiting to maintenance/raw REPL. This
        # matches pico-sdk watchdog_disable() and allows safe USB file updates.
        # See docs/TWO_BRAIN_PROTOCOL.md for register source references.
        mem32[0x4005B000] = 0x40000000
    if controller is not None:
        controller.close()
    elif uart is not None:
        uart.deinit()
    print("UART CONTROL: motors stopped")
