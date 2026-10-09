import unittest
from circle_show import CircleShow
from test_uart_control import Clock, RGBLEDs, DeferredFace


class CircleShowTests(unittest.TestCase):
    def test_happy_excited_love_faces_and_color_chase_restore(self):
        clock = Clock(); face = DeferredFace(clock); leds = RGBLEDs()
        leds.set(0, (5, 6, 7)); leds.set_brightness(8, led=0)
        show = CircleShow(face, leds, clock)
        show.update(0)
        colors = [c[:] for c in leds.colors]
        for stamp in (180, 1200, 2400, 3600):
            clock.now = stamp; show.update(stamp)
        self.assertIn(("happy", "NONE"), face.actions)
        self.assertIn(("excited", "NONE"), face.actions)
        self.assertIn(("love", "NONE"), face.actions)
        self.assertNotEqual(leds.colors, colors)
        show.cancel()
        self.assertEqual(leds.colors[0], [5, 6, 7])
        self.assertEqual(leds.brightness[0], 8)
        self.assertEqual(face.actions[-1], "neutral")

    def test_decoration_failure_is_contained(self):
        clock = Clock(); face = DeferredFace(clock); leds = RGBLEDs()
        show = CircleShow(face, leds, clock)
        def fail(): raise OSError("LED unavailable")
        leds.show = fail
        show.update(0)
        self.assertTrue(show.failed)
        show.update(200)  # No exception or repeated retries in the motor loop.
        show.cancel()
        self.assertFalse(show.failed)
