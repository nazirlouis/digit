"""Desktop transport tests. No device access and no motor operations."""

import importlib.util
import pathlib
import sys
import time
import types
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Pin:
    OUT, IN, ALT, PULL_UP = 1, 0, 2, 3
    pins = {}

    def __new__(cls, number, *args, **kwargs):
        if number not in cls.pins:
            obj = super().__new__(cls)
            obj.number, obj.mode, obj.level = number, cls.IN, 0
            cls.pins[number] = obj
        return cls.pins[number]

    def __init__(self, number, mode=None, **kwargs):
        if mode is not None:
            self.init(mode, **kwargs)

    def init(self, mode, value=None, **kwargs):
        self.mode = mode
        if value is not None:
            self.level = value

    def __call__(self, value=None):
        if value is not None:
            self.level = value
        return self.level


class SPI:
    instance = None

    def __new__(cls, *args, **kwargs):
        if cls.instance is None:
            cls.instance = super().__new__(cls)
            cls.instance.writes = []
            cls.instance.fail_next = False
        return cls.instance

    def __init__(self, id, baudrate, sck, mosi, miso=None, **kwargs):
        assert id == 0
        for pin in (sck, mosi, miso):
            if pin is not None:
                pin.init(Pin.ALT)
        self.init(baudrate=baudrate, **kwargs)

    def init(self, **kwargs):
        self.settings = kwargs

    def write(self, data):
        if self.fail_next:
            self.fail_next = False
            raise OSError("injected SPI failure")
        lcd_clock = Pin(2).mode == Pin.ALT
        led_clock = Pin(6).mode == Pin.ALT
        assert not (lcd_clock and led_clock), "LCD and LEDs clocked together"
        if lcd_clock:
            assert Pin(7)() == 0, "LCD write with CS deselected"
            assert Pin(0).mode == Pin.OUT, "DC was not restored after button read"
            self.writes.append((Pin(0)(), bytes(data)))
        else:
            assert led_clock, "No SPI clock selected"
            assert Pin(7)() == 1, "LED write selected the LCD"


class FrameBuffer:
    """Only the native buffer operations required by transport tests."""

    def __init__(self, buffer, width, height, fmt):
        self.buf, self.w, self.h = buffer, width, height

    def pixel(self, x, y, value=None):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return 0
        index = y * (self.w // 8) + x // 8
        bit = 128 >> (x % 8)
        if value is None:
            return 1 if self.buf[index] & bit else 0
        if value:
            self.buf[index] |= bit
        else:
            self.buf[index] &= ~bit

    def fill(self, value):
        self.buf[:] = bytes((255 if value else 0,)) * len(self.buf)

    def fill_rect(self, x, y, width, height, value):
        for py in range(y, y + height):
            for px in range(x, x + width):
                self.pixel(px, py, value)

    def line(self, x0, y0, x1, y1, value):
        dx = abs(x1 - x0)
        sx = 1 if x0 < x1 else -1
        dy = -abs(y1 - y0)
        sy = 1 if y0 < y1 else -1
        error = dx + dy
        while True:
            self.pixel(x0, y0, value)
            if x0 == x1 and y0 == y1:
                return
            doubled = 2 * error
            if doubled >= dy:
                error += dy
                x0 += sx
            if doubled <= dx:
                error += dx
                y0 += sy

    def ellipse(self, center_x, center_y, radius_x, radius_y, value, fill=False):
        if radius_x <= 0 or radius_y <= 0:
            return
        limit = radius_x * radius_x * radius_y * radius_y
        for y in range(center_y - radius_y, center_y + radius_y + 1):
            for x in range(center_x - radius_x, center_x + radius_x + 1):
                dx = x - center_x
                dy = y - center_y
                score = (dx * dx * radius_y * radius_y +
                         dy * dy * radius_x * radius_x)
                if score <= limit and (fill or score * 4 >= limit * 3):
                    self.pixel(x, y, value)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DisplayTests(unittest.TestCase):
    def setUp(self):
        Pin.pins = {}
        SPI.instance = None
        modules = {
            "machine": types.SimpleNamespace(Pin=Pin, SPI=SPI),
            "framebuf": types.SimpleNamespace(FrameBuffer=FrameBuffer, MONO_HLSB=3),
            "micropython": types.SimpleNamespace(const=lambda value: value),
        }
        patches = [patch.dict(sys.modules, modules),
                   patch.object(time, "sleep_ms", lambda _: None, create=True),
                   patch.object(time, "ticks_ms", lambda: time.monotonic_ns() // 1_000_000, create=True),
                   patch.object(time, "ticks_add", lambda a, b: a+b, create=True),
                   patch.object(time, "ticks_us", lambda: time.monotonic_ns() // 1000, create=True),
                   patch.object(time, "ticks_diff", lambda a, b: a-b, create=True),
                   patch("builtins.print")]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)
        self.module = load("display_under_test", ROOT / "digit_display_lcd.py")
        self.display = self.module.Display()
        self.spi = self.display.spi

    def assert_idle(self):
        self.assertEqual(Pin(7)(), 1)
        self.assertNotIn(28, Pin.pins)
        self.assertNotIn(29, Pin.pins)
        self.assertEqual(Pin(2).mode, Pin.OUT)
        self.assertEqual(Pin(2)(), 0)
        self.assertEqual(Pin(0)(), 0)

    def frame_data(self):
        writes = self.spi.writes
        start = next(i for i, value in enumerate(writes) if value == (0, b"\x2c"))
        return b"".join(data for dc, data in writes[start + 1:])

    def test_initialization_panel_window_and_idle_bus(self):
        writes = self.spi.writes
        self.assertEqual(writes[:4], [(0, b"\x36"), (1, b"\x70"),
                                     (0, b"\x3a"), (1, b"\x05")])
        self.assertIn((0, b"\x21"), writes)
        self.assertIn((0, b"\x11"), writes)
        self.assertIn((0, b"\x29"), writes)
        index = writes.index((0, b"\x2a"))
        self.assertEqual(writes[index+1], (1, b"\x00\x14\x01\x2b"))
        self.assertEqual(writes[index+3], (1, b"\x00\x00\x00\xef"))
        self.assertEqual(len(self.frame_data()), 280 * 240 * 2)
        self.assert_idle()

    def test_scaled_corner_pixels_and_inclusive_canvas_window(self):
        self.spi.writes.clear()
        self.display.pixel(0, 0, 1)
        self.display.pixel(127, 63, 1)
        self.display.show()
        self.assertEqual(self.spi.writes[:4], [
            (0, b"\x2a"), (1, b"\x00\x20\x01\x1f"),
            (0, b"\x2b"), (1, b"\x00\x38\x00\xb7")])
        data = self.frame_data()
        expected = bytearray(256 * 128 * 2)
        for y in (0, 1):
            expected[y*512:y*512+4] = b"\xff" * 4
        for y in (126, 127):
            expected[y*512+508:y*512+512] = b"\xff" * 4
        self.assertEqual(data, expected)
        self.assert_idle()

    def test_unchanged_frame_does_not_use_spi(self):
        self.display.show()
        self.spi.writes.clear()
        self.display.show()
        self.assertEqual(self.spi.writes, [])
        self.assertEqual(self.display.last_rows_sent, 0)

    def test_changed_row_and_erasure(self):
        self.display.show()
        for color in (1, 0):
            self.spi.writes.clear()
            self.display.pixel(7, 10, color)
            self.display.show()
            self.assertEqual(self.spi.writes[3], (1, b"\x00\x4c\x00\x4d"))
            self.assertEqual(len(self.frame_data()), 64)
            self.assertEqual(self.display.last_rows_sent, 2)
            self.assertEqual(self.display.last_bytes_sent, 64)
        self.assertEqual(self.frame_data(), bytes(64))

    def test_rgb565_wire_order_and_bit_order(self):
        self.display.set_colors(0xF800, 0x001F)
        self.display.pixel(7, 0, 1)
        self.spi.writes.clear()
        self.display.show()
        self.assertEqual(self.frame_data()[:32], b"\x00\x1f" * 14 + b"\xf8\x00" * 2)

    def test_force_update_and_clear_invalidate_previous_frame(self):
        self.display.show()
        self.spi.writes.clear()
        self.display.show(full_update=True)
        self.assertEqual(len(self.frame_data()), 65536)
        self.display.clear_panel(0xF800)
        self.spi.writes.clear()
        self.display.show()
        self.assertEqual(len(self.frame_data()), 65536)

    def test_color_change_resends_unchanged_pixels_in_the_new_palette(self):
        self.display.pixel(0, 0, 1)
        self.display.show()
        previous_pixels = bytes(self.display.buffer)
        self.spi.writes.clear()
        self.display.set_colors(0xF800, 0)
        self.display.show()
        self.assertEqual(bytes(self.display.buffer), previous_pixels)
        self.assertEqual(self.frame_data()[:8], b"\xf8\x00" * 2 + b"\x00" * 4)
        self.assertEqual(len(self.frame_data()), 65536)
        self.assert_idle()

    def test_palette_reuses_small_buffers_across_all_expression_colors(self):
        from digit_face_engine import COLORS, SURPRISE_FLASH_COLOR
        lookup = self.display._lookup
        blocks = tuple(lookup)
        self.assertEqual(sum(len(block) for block in blocks), 256)
        # Color changes must not allocate a new table, pixel bytes or buffers.
        def unexpected_allocation(*args, **kwargs):
            self.fail("palette rebuilt a container during color change")
        with patch.object(self.module, "tuple", unexpected_allocation, create=True), \
             patch.object(self.module, "bytes", unexpected_allocation, create=True), \
             patch.object(self.module, "bytearray", unexpected_allocation, create=True):
            for _ in range(20):
                for color in tuple(COLORS.values()) + (SURPRISE_FLASH_COLOR,):
                    self.display.set_colors(color, 0)
                    self.assertIs(self.display._lookup, lookup)
                    for old, current in zip(blocks, self.display._lookup):
                        self.assertIs(current, old)
        self.display.show()
        self.spi.writes.clear()
        self.display.set_colors(SURPRISE_FLASH_COLOR, 0)
        self.display.show()
        self.assertEqual(self.spi.writes, [])

    def test_all_source_bytes_expand_correctly_after_palette_changes(self):
        for foreground, background in ((0xF800, 0), (0x07FF, 0x1234)):
            self.display.set_colors(foreground, background)
            # The first 16 source rows contain every possible byte value.
            self.display.buffer[:256] = bytes(range(256))
            self.spi.writes.clear()
            self.display.show()
            wire = self.frame_data()
            for row in range(16):
                expected = bytearray()
                for value in range(row * 16, (row + 1) * 16):
                    for bit in range(7, -1, -1):
                        color = foreground if value & (1 << bit) else background
                        expected.extend(bytes((color >> 8, color & 255)) * 2)
                start = row * 1024
                self.assertEqual(wire[start:start + 1024], bytes(expected) * 2)

    def test_transfer_failure_releases_bus_and_forces_retry(self):
        self.display.show()
        self.display.pixel(2, 20, 1)
        self.spi.fail_next = True
        with self.assertRaises(OSError):
            self.display.show()
        self.assert_idle()
        self.display.pixel(2, 20, 0)
        self.spi.writes.clear()
        self.display.show()
        self.assertEqual(len(self.frame_data()), 65536)

    def test_actual_pololu_rgb_driver_and_button_pin_reconfiguration(self):
        rgb = load("pololu_rgb", ROOT / "factory-backup/zumo_2040_robot/rgb_leds.py")
        leds = rgb.RGBLEDs()
        self.display.show()
        self.assert_idle()
        leds.set(0, [10, 20, 30])
        leds.show()
        # ButtonC temporarily reads GP0 as an input between display frames.
        Pin(0).init(Pin.IN)
        self.display.pixel(0, 0, 1)
        self.display.show()
        self.assert_idle()
        leds.off()

    def test_flipped_landscape_keeps_canvas_window(self):
        self.module.FLIP_180 = True
        self.spi.writes.clear()
        display = self.module.Display()
        self.assertEqual(self.spi.writes[1], (1, b"\xb0"))
        self.spi.writes.clear()
        display.show()
        self.assertEqual(self.spi.writes[1], (1, b"\x00\x20\x01\x1f"))

    def test_disabled_buzzer_preserves_lcd_cs_during_and_after_transfer(self):
        # Import and menu cleanup must not reclaim GP7 or change its level.
        for level in (0, 1):
            Pin(7)(level)
            before = {n: (p.mode, p.level) for n, p in Pin.pins.items()}
            module = load("silent_buzzer", ROOT / "device-library/zumo_2040_robot/buzzer.py")
            buzzer = module.Buzzer()
            buzzer.set_callback(lambda _: self.fail("Silent buzzer called callback"))
            buzzer.beep()
            buzzer.on()
            buzzer.play("c")
            buzzer.play_in_background("c")
            buzzer.off()
            self.assertFalse(buzzer.is_playing())
            self.assertEqual(before, {n: (p.mode, p.level) for n, p in Pin.pins.items()})
        self.display.show()
        self.assert_idle()

    def test_large_face_canvas_uses_nearly_the_whole_panel(self):
        self.spi.writes.clear()
        display = self.module.Display(width=136, height=112, foreground=0xFCE0)
        self.spi.writes.clear()
        display.pixel(0, 0, 1)
        display.pixel(135, 111, 1)
        display.show()
        self.assertEqual(self.spi.writes[:4], [
            (0, b"\x2a"), (1, b"\x00\x18\x01\x27"),
            (0, b"\x2b"), (1, b"\x00\x08\x00\xe7")])
        self.assertEqual(len(self.frame_data()), 272 * 224 * 2)
        self.assertEqual(display.last_bytes_sent, 272 * 224 * 2)

    def test_high_resolution_face_geometry_and_orange_palette(self):
        with patch.dict(sys.modules, {"digit_display": self.module}):
            face_module = load("face_under_test", ROOT / "digit_face_lcd.py")
            face = face_module.DigitFace()
        self.assertEqual((face.display.width, face.display.height), (136, 112))
        self.assertEqual(face.CORNER_RADIUS, 24)
        self.assertEqual(face.BLINK_DURATION_MS, 110)
        self.assertEqual(face._blink_height(0), 70)
        self.assertEqual(face._blink_height(55), 6)
        self.assertEqual(face._blink_height(110), 70)
        self.assertEqual(face.display.pixel(36, 56), 1)
        self.assertEqual(face.display.pixel(100, 56), 1)
        self.assertEqual(face.display.pixel(12, 21), 0)
        self.assertEqual(face.display._lookup[0x8][:4], b"\xfc\xe0" * 2)
        self.assertGreater(face.display.last_bytes_sent, 0)


if __name__ == "__main__":
    unittest.main()
