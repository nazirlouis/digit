"""Minimal, motor-safe diagnostic for Digit's Waveshare 1.69-inch LCD.

Run this file directly from Thonny.  It does not import the face code and does
not command the motors.  The test uses a slow SPI clock and tries mode 0 first,
then mode 3, because Waveshare's Raspberry Pi reference uses mode 0 while its
Arduino reference configures mode 3.

Expected result: the entire display changes color during at least one mode.
If the backlight stays lit but every color remains black in both modes, inspect
CS, DC, RST, DIN and CLK wiring; the write-only LCD cannot acknowledge them.
"""

from machine import Pin, SPI
import time


SCK_PIN = 2
MOSI_PIN = 3
DC_PIN = 0
RESET_PIN = 1
CS_PIN = 7  # Buzzer jumper disconnected; install the disabled buzzer library.
SPI_BAUDRATE = 1_000_000

WIDTH = 280
HEIGHT = 240


def pin_level(pin):
    try:
        return pin.value()
    except AttributeError:
        return pin()


class RawLCD:
    def __init__(self, polarity, phase):
        self.bytes_sent = 0
        self.cs = Pin(CS_PIN, Pin.OUT, value=1)
        self.dc = Pin(DC_PIN, Pin.OUT, value=0)
        self.reset = Pin(RESET_PIN, Pin.OUT, value=1)
        self.sck = Pin(SCK_PIN)
        self.spi = SPI(
            0,
            baudrate=SPI_BAUDRATE,
            polarity=polarity,
            phase=phase,
            sck=self.sck,
            mosi=Pin(MOSI_PIN),
            miso=None,
        )
        print(
            "LCD RAW: pins CLK=GP{} DIN=GP{} DC=GP{} RST=GP{} CS=GP{}".format(
                SCK_PIN, MOSI_PIN, DC_PIN, RESET_PIN, CS_PIN
            )
        )
        print(
            "LCD RAW: idle readback CS={} DC={} RST={}".format(
                pin_level(self.cs), pin_level(self.dc), pin_level(self.reset)
            )
        )

    def write(self, data):
        self.spi.write(data)
        self.bytes_sent += len(data)

    def command(self, command, data=None):
        self.dc(0)
        self.write(bytes((command,)))
        if data is not None:
            self.dc(1)
            self.write(data)

    def initialize(self):
        print("LCD RAW: hardware reset high -> low -> high")
        self.cs(0)
        self.reset(1)
        time.sleep_ms(100)
        self.reset(0)
        time.sleep_ms(100)
        self.reset(1)
        time.sleep_ms(100)

        settings = (
            (0x36, b"\x70"),
            (0x3A, b"\x05"),
            (0xB2, b"\x0b\x0b\x00\x33\x35"),
            (0xB7, b"\x11"),
            (0xBB, b"\x35"),
            (0xC0, b"\x2c"),
            (0xC2, b"\x01"),
            (0xC3, b"\x0d"),
            (0xC4, b"\x20"),
            (0xC6, b"\x13"),
            (0xD0, b"\xa4\xa1"),
            (0xD6, b"\xa1"),
            (0xE0, b"\xf0\x06\x0b\x0a\x09\x26\x29\x33\x41\x18\x16\x15\x29\x2d"),
            (0xE1, b"\xf0\x04\x08\x08\x07\x03\x28\x32\x40\x3b\x19\x18\x2a\x2e"),
            (0xE4, b"\x25\x00\x00"),
            (0x21, None),
            (0x11, None),
        )
        for command, data in settings:
            self.command(command, data)
        time.sleep_ms(120)
        self.command(0x29)
        time.sleep_ms(20)
        self.cs(1)
        print("LCD RAW: init complete; {} bytes sent".format(self.bytes_sent))

    def fill(self, name, color):
        started = time.ticks_ms()
        self.cs(0)
        self.command(0x2A, b"\x00\x14\x01\x2b")
        self.command(0x2B, b"\x00\x00\x00\xef")
        self.command(0x2C)
        self.dc(1)
        row = bytes((color >> 8, color & 255)) * WIDTH
        for _ in range(HEIGHT):
            self.write(row)
        self.cs(1)
        elapsed = time.ticks_diff(time.ticks_ms(), started)
        print(
            "LCD RAW: {} fill complete; {} pixel bytes in {} ms".format(
                name, WIDTH * HEIGHT * 2, elapsed
            )
        )

    def idle(self):
        self.cs(1)
        self.dc(0)
        print(
            "LCD RAW: final readback CS={} DC={} RST={}".format(
                pin_level(self.cs), pin_level(self.dc), pin_level(self.reset)
            )
        )


def run_mode(polarity, phase, label):
    print("\nLCD RAW: starting {} at {} Hz".format(label, SPI_BAUDRATE))
    lcd = RawLCD(polarity, phase)
    try:
        lcd.initialize()
        for name, color in (
            ("RED", 0xF800),
            ("GREEN", 0x07E0),
            ("BLUE", 0x001F),
            ("WHITE", 0xFFFF),
        ):
            print("LCD RAW: look for", name)
            lcd.fill(name, color)
            time.sleep_ms(1500)
    finally:
        lcd.idle()


def run():
    print("LCD RAW TEST: motors are not commanded")
    print("LCD RAW TEST: BL must already be connected to 3V3")
    run_mode(0, 0, "SPI MODE 0")
    time.sleep_ms(1000)
    run_mode(1, 1, "SPI MODE 3")
    print("\nLCD RAW TEST: finished; the screen should now be WHITE")


if __name__ == "__main__":
    run()
