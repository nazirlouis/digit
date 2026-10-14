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

    def test_legacy_and_digit_imports_use_lcd_even_with_stale_screen_config(self):
        with patch.dict(sys.modules, {
            "digit_display_lcd": self.module,
            "digit_config": types.SimpleNamespace(DISPLAY_TYPE="oled"),
        }):
            active = load("active_display", ROOT / "digit_display.py")
            legacy = load("legacy_display", ROOT / "device-library" /
                          "zumo_2040_robot" / "display.py")
            self.assertIs(active.Display, self.module.Display)
            self.assertIs(legacy.Display, self.module.Display)
            with patch.dict(sys.modules, {"digit_display": active}):
                lcd_face = load("lcd_face_adapter", ROOT / "digit_face_lcd.py")
                with patch.dict(sys.modules, {"digit_face_lcd": lcd_face}):
                    face = load("active_face", ROOT / "digit_face.py")
                self.assertIs(face.DigitFace, lcd_face.DigitFace)

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

    def drain(self):
        for _ in range(self.display.height + 1):
            if not self.display.frame_pending:
                return
            self.display.flush(max_rows=2, max_us=1_000_000)
        self.fail("cooperative display never completed")

    def test_cooperative_show_only_queues_and_flush_is_bounded(self):
        self.display.enable_cooperative()
        self.spi.writes.clear()
        self.display.pixel(0, 0, 1)
        frame_id = self.display.show()
        self.assertEqual(self.spi.writes, [])
        self.assertTrue(self.display.frame_pending)
        self.display.flush(max_rows=2, max_us=1_000_000)
        self.assertEqual(self.display.last_rows_sent, 4)
        self.assertEqual(self.display._scan_row, 2)
        self.assertEqual(self.display.completed_frame_id, 0)
        self.assert_idle()
        self.drain()
        self.assertEqual(self.display.completed_frame_id, frame_id)
        self.assertEqual(self.display._previous, self.display.buffer)
        self.assertGreaterEqual(self.display.max_slice_us, self.display.last_slice_us)

    def test_new_drawing_does_not_mutate_queued_pixels_or_palette(self):
        self.display.enable_cooperative()
        self.display.fill(1)
        self.display.set_colors(0xF800, 0)
        self.display.show()
        self.display.fill(0)
        self.display.set_colors(0x07FF, 0)
        self.spi.writes.clear()
        self.display.flush(max_rows=1, max_us=1_000_000)
        self.assertEqual(self.frame_data(), b"\xf8\x00" * (128 * 4))
        self.assertEqual(self.display._foreground, 0xF800)
        self.display.show()
        self.drain()
        self.assertFalse(any(self.display._previous))
        self.assertEqual(self.display._foreground, 0x07FF)

    def test_replaced_partial_frame_repairs_erasures_and_all_palette_rows(self):
        self.display.enable_cooperative()
        self.display.fill(1)
        self.display.show()
        self.display.flush(max_rows=20, max_us=1_000_000)
        self.display.fill(0)
        self.display.pixel(127, 63, 1)
        self.display.set_colors(0xF800, 0)
        replacement = self.display.show()
        self.drain()
        self.assertEqual(self.display._previous, self.display.buffer)
        self.assertTrue(all(self.display._row_valid))
        self.assertEqual(self.display.completed_frame_id, replacement)

    def test_cooperative_failure_retry_and_cancellation_release_shared_pins(self):
        self.display.enable_cooperative()
        self.display.show()
        self.spi.fail_next = True
        with self.assertRaises(OSError):
            self.display.flush()
        self.assertEqual(self.display._scan_row, 0)
        self.assertFalse(self.display._row_valid[0])
        self.assert_idle()
        rgb = load("cooperative_rgb", ROOT / "factory-backup/zumo_2040_robot/rgb_leds.py")
        leds = rgb.RGBLEDs()
        leds.show()
        Pin(0).init(Pin.IN)
        self.display.flush(max_rows=2, max_us=1_000_000)
        self.assert_idle()
        self.display.cancel_frame()
        self.spi.writes.clear()
        self.display.flush()
        self.assertEqual(self.spi.writes, [])
        self.assertEqual(self.display.completed_frame_id, 0)
        self.display.show()
        self.drain()

    def test_cooperative_time_budget_checks_between_indivisible_rows(self):
        self.display.enable_cooperative()
        self.display.show()
        ticks = iter((0, 2000, 2100))
        with patch.object(time, "ticks_us", lambda: next(ticks)):
            self.display.flush(max_rows=16, max_us=1500)
        self.assertEqual(self.display._scan_row, 1)
        self.assertEqual(self.display.last_slice_us, 2100)
        self.assert_idle()

    def test_frequently_replaced_frames_do_not_starve_lower_panel_rows(self):
        self.display.enable_cooperative()
        self.display.fill(1)
        for row in range(self.display.height):
            self.display.show()
            self.display.flush(max_rows=1, max_us=1_000_000)
            self.assertTrue(self.display._row_valid[row])
        self.assertEqual(self.display._previous, self.display.buffer)
        self.drain()
        self.assertEqual(self.display.completed_frame_id, self.display.frame_id)

    def test_palette_changed_mid_transfer_tracks_actual_color_of_each_row(self):
        self.display.fill(1)
        self.display.show()
        self.display.enable_cooperative()
        self.display.set_colors(0xF800, 0)
        self.display.show()
        self.display.flush(max_rows=2, max_us=1_000_000)
        self.assertEqual(self.display._row_foreground[:3], [0xF800, 0xF800, 0xFFFF])
        self.display.set_colors(0x07FF, 0x001F)
        self.display.fill(0)
        self.display.show()
        self.drain()
        self.assertTrue(all(color == 0x07FF for color in self.display._row_foreground))
        self.assertTrue(all(color == 0x001F for color in self.display._row_background))
        self.assertFalse(any(self.display._previous))


if __name__ == "__main__":
    unittest.main()
