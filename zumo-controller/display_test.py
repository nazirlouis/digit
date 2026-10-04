"""Run manually on the Zumo: LCD screen and buttons; no motor commands."""

import gc
import time
from digit_display import Display
from digit_face import DigitFace
from zumo_2040_robot import robot


def run():
    display = Display(
        width=DigitFace.CANVAS_WIDTH,
        height=DigitFace.CANVAS_HEIGHT,
        foreground=DigitFace.ORANGE,
    )
    leds = robot.RGBLEDs()
    button_a = robot.ButtonA()
    button_b = robot.ButtonB()
    button_c = robot.ButtonC()
    print("LCD TEST: red, green, blue, white, black; then animated eyes")
    print("LCD TEST: A=sad, B=happy, C=exit; motors are not commanded")
    print("LCD TEST: verify colors/orientation visually; SPI writes cannot confirm them")
    try:
        for name, color in (("RED", 0xF800), ("GREEN", 0x07E0),
                            ("BLUE", 0x001F), ("WHITE", 0xFFFF), ("BLACK", 0)):
            print("LCD TEST:", name)
            display.clear_panel(color)
            time.sleep_ms(500)
        display.fill(0)
        display.rect(0, 0, display.width, display.height, 1)
        display.text("TOP LEFT", 2, 2, 1)
        display.text("BOTTOM RIGHT", 34, display.height - 10, 1)
        display.show()
        print("LCD TEST: full border and both labels should be visible")
        time.sleep_ms(2000)
        face = DigitFace(display=display)
        last_log = time.ticks_ms()
        frames = 0
        screen_updates = 0
        bytes_sent = 0
        worst_show_us = 0
        led_on = False
        while not button_c.check():
            if button_a.check() is True:
                face.play_sad()
                print("LCD TEST: A / sad")
            if button_b.check() is True:
                face.play_happy()
                print("LCD TEST: B / happy")
            face.update()
            frames += 1
            if display.last_rows_sent:
                screen_updates += 1
                bytes_sent += display.last_bytes_sent
                worst_show_us = max(worst_show_us, display.last_show_us)
            now = time.ticks_ms()
            if time.ticks_diff(now, last_log) >= 1000:
                led_on = not led_on
                leds.set(0, [0, 32 if led_on else 0, 0])
                leds.show()
                print(
                    "LCD TEST: loops={} updates={} bytes={} max_show={}us free={}B".format(
                        frames,
                        screen_updates,
                        bytes_sent,
                        worst_show_us,
                        gc.mem_free(),
                    )
                )
                frames = 0
                screen_updates = 0
                bytes_sent = 0
                worst_show_us = 0
                last_log = now
            time.sleep_ms(20)
        print("LCD TEST: C / stopped")
    finally:
        leds.off()
        display.clear_panel()


if __name__ == "__main__":
    run()
