"""Run directly in Thonny: LCD proximity demo with serial startup checkpoints."""

import time
import math
from array import array


def run():
    print("PROX LCD v2: starting")
    # Import only the needed hardware modules, avoiding robot's buzzer import.
    from zumo_2040_robot.motors import Motors
    motors = Motors()
    motors.off()
    display = None
    proximity_sensors = None
    stage = "LCD import/init"
    try:
        from digit_display_lcd import Display
        print("PROX LCD: initializing ST7789V2; CS=GP7")
        display = Display()
        display.fill(0)
        display.text("PROX LCD v2", 0, 0, 1)
        display.text("Starting sensors", 0, 20, 1)
        display.text("B: exit", 0, 54, 1)
        display.show()
        print("PROX LCD: startup frame sent; check panel visually")
        time.sleep_ms(750)
        stage = "sensor import/init"
        from zumo_2040_robot.proximity_sensors import ProximitySensors
        from zumo_2040_robot.buttons import ButtonB
        proximity_sensors = ProximitySensors()
        button_b = ButtonB()
        last_log = time.ticks_ms()
        print("PROX LCD: sensors ready; starting readings")
        stage = "sensor read/draw"
        while button_b.check() is not True:
            start = time.ticks_us()
            proximity_sensors.read()
            stop = time.ticks_us()

            display.fill(0)
            display.text("{:.1f}ms".format(time.ticks_diff(stop, start) / 1000), 0, 0)
            display.text("<B", 112, 0)
            count = 0

            readings = [
                proximity_sensors.left_counts_with_left_leds(),
                proximity_sensors.left_counts_with_right_leds(),
                0,
                proximity_sensors.front_counts_with_left_leds(),
                proximity_sensors.front_counts_with_right_leds(),
                0,
                proximity_sensors.right_counts_with_left_leds(),
                proximity_sensors.right_counts_with_right_leds(),
            ]

            if proximity_sensors.total_counts() > 0:
                angle_estimate = proximity_sensors.angle_estimate()
                display.text(str(angle_estimate), 0, 10)

                dx = round(25 * math.sin(angle_estimate * math.pi / 180))
                dy = round(-25 * math.cos(angle_estimate * math.pi / 180))
                dx2 = round(4 * math.sin((angle_estimate + 90) * math.pi / 180))
                dy2 = round(-4 * math.cos((angle_estimate + 90) * math.pi / 180))
                display.poly(64, 32,
                             array('h', [dx2, dy2, dx, dy, -dx2, -dy2]), 1, 1)

            # 64-40 = 24
            scale = 24/6

            for i, reading in enumerate(readings):
                display.fill_rect(18+i*12, 64-int(reading*scale), 8, int(reading*scale), 1)

            display.show()
            if time.ticks_diff(time.ticks_ms(), last_log) >= 1000:
                print("PROX LCD: L={} F={} R={}".format(
                    readings[:2], readings[3:5], readings[6:]))
                last_log = time.ticks_ms()
            time.sleep_ms(50)
        display.fill(0)
        display.text("Proximity stopped", 0, 20, 1)
        display.show()
        print("PROX LCD: stopped")
    except Exception as error:
        print("PROX LCD: failed at", stage, type(error).__name__, str(error))
        if display is not None:
            try:
                display.fill(0)
                display.text("PROX ERROR", 0, 0, 1)
                display.text("See Thonny Shell", 0, 20, 1)
                display.show()
            except Exception as screen_error:
                print("PROX LCD: error screen failed:", screen_error)
        raise
    finally:
        motors.off()
        if proximity_sensors is not None:
            proximity_sensors.ir_pulses.off()


if __name__ == "__main__":
    run()
