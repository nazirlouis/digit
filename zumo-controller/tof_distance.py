"""Show VL53L4CD distance on Digit's Waveshare LCD and serial console.

Wiring on the Zumo 2040 expansion header:
    VL53L4CD VIN -> 3V3
    VL53L4CD GND -> GND
    VL53L4CD SDA -> SDA / GP4
    VL53L4CD SCL -> SCL / GP5

Copy this file and digit_display.py to the Zumo root and place vl53l4cd.py in /zumo_2040_robot,
then run tof_distance.py.
Press button C to stop.
"""

from machine import I2C, Pin
from zumo_2040_robot import robot
from digit_display import Display
import time

from zumo_2040_robot.vl53l4cd import VL53L4CD


I2C_ADDRESS = 0x29
UPDATE_INTERVAL_MS = 100


def show(display, title, value=""):
    display.fill(0)
    display.text(title, max(0, (128 - len(title) * 8) // 2), 14, 1)
    if value:
        display.text(value, max(0, (128 - len(value) * 8) // 2), 36, 1)
    display.show()


display = Display()
button_c = robot.ButtonC()
i2c = I2C(id=0, scl=Pin(5), sda=Pin(4), freq=400_000)
sensor = None
devices = []
failed = False

try:
    devices = i2c.scan()
    print("I2C devices:", ["0x{:02X}".format(address) for address in devices])

    if I2C_ADDRESS not in devices:
        show(display, "TOF NOT FOUND", "Expected 0x29")
        raise RuntimeError(
            "VL53L4CD not found at 0x29; check 3V3, GND, SDA GP4, and SCL GP5"
        )

    show(display, "STARTING TOF")
    sensor = VL53L4CD(i2c, I2C_ADDRESS)
    sensor.start_ranging()
    print("VL53L4CD ready. Press button C to stop.")

    last_update = time.ticks_add(time.ticks_ms(), -UPDATE_INTERVAL_MS)

    while not button_c.check():
        now = time.ticks_ms()
        if sensor.data_ready:
            distance_mm = sensor.distance_mm
            status = sensor.range_status
            sensor.clear_interrupt()

            if time.ticks_diff(now, last_update) >= UPDATE_INTERVAL_MS:
                last_update = now
                if status == 0:
                    value = "{} mm".format(distance_mm)
                    show(display, "DISTANCE", value)
                    print(value)
                else:
                    value = "Status {}".format(status)
                    show(display, "NO VALID RANGE", value)
                    print("Invalid range; sensor status:", status)

        time.sleep_ms(5)

except Exception as error:
    failed = True
    if I2C_ADDRESS in devices:
        show(display, "TOF ERROR", "See console")
    print("VL53L4CD error:", error)
    raise

finally:
    if sensor is not None:
        sensor.stop_ranging()
    if not failed:
        show(display, "TOF STOPPED")
        print("VL53L4CD stopped.")
