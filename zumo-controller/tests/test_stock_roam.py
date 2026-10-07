"""Desktop checks for stock tabletop motion decisions (no physical claims)."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("stock_roam", Path(__file__).parents[1] / "stock_roam.py")
roam = importlib.util.module_from_spec(spec)
spec.loader.exec_module(roam)
roam.time.ticks_add = lambda a, b: (a + b) % (1 << 30)
roam.time.ticks_diff = lambda a, b: ((a - b + (1 << 29)) % (1 << 30)) - (1 << 29)


class StockRoamTests(unittest.TestCase):
    def test_reject_dark_and_unstable_calibration(self):
        for rows in ([[900] * 5] * 60, [[100] * 5, [250] * 5] * 30):
            with self.assertRaises(ValueError):
                roam.thresholds_for(rows)

    def test_every_sensor_triggers_at_threshold(self):
        thresholds = roam.thresholds_for([[150] * 5] * 60)
        for i in range(5):
            values = [150] * 5
            values[i] = thresholds[i]
            self.assertTrue(roam.edge_mask(values, thresholds)[i])

    def test_stop_reverse_turn_clear(self):
        r = roam.Roamer(0)
        self.assertEqual(r.step(0, [False] * 5), (2500, 2500))
        self.assertEqual(r.step(10, [True, False, False, False, False]), (0, 0))
        self.assertEqual(r.step(12, [True] * 5), (-1100, -1100))
        r.step(30, [False] * 5)
        self.assertEqual(r.step(115, [False] * 5), (0, 0))
        self.assertEqual(r.state, "edge_turn")
        self.assertEqual(r.step(117, [False] * 5), (1500, -1500))
        self.assertEqual(r.step(118, [True] * 5), (0, 0))
        self.assertEqual(r.state, "reverse")
        r.step(120, [False] * 5)
        r.step(205, [False] * 5)
        self.assertEqual(r.step(r.until, [False] * 5), (2500, 2500))
        self.assertEqual(r.state, "cruise")
        self.assertEqual(r.recoveries, 0)

    def test_repeated_failed_recovery_stops(self):
        r = roam.Roamer(0)
        for now in (10, 110, 210):
            r.step(now, [True] * 5)
            r.step(now + 1, [False] * 5)
            r.step(now + 86, [False] * 5)
            self.assertEqual(r.state, "edge_turn")
        self.assertEqual(r.step(310, [True] * 5), (0, 0))
        self.assertEqual(r.state, "failed")
        self.assertEqual(r.step(10000, [False] * 5), (0, 0))

    def test_turn_choices_and_more_frequent_cruise(self):
        from unittest.mock import patch
        for duration in (250, 650, 1250):
            r = roam.Roamer(0)
            self.assertTrue(800 <= r.until <= 1800)
            start = r.until
            with patch.object(roam.random, "choice", side_effect=[1, duration]):
                r.step(start, [False] * 5)
            self.assertEqual(r.state, "turn")
            self.assertEqual(r.until - start, duration)
            self.assertEqual(r.step(r.until, [False] * 5), (2500, 2500))

    def test_reverse_timeout(self):
        r = roam.Roamer(0)
        r.step(1, [True] * 5)
        self.assertEqual(r.step(351, [True] * 5), (0, 0))
        self.assertEqual(r.state, "failed")

    def test_edge_during_random_turn_and_tick_wrap(self):
        r = roam.Roamer((1 << 30) - 5)
        r.until = 5
        r.step(6, [False] * 5)
        self.assertEqual(r.state, "turn")
        self.assertEqual(r.step(7, [False, False, False, False, True]), (0, 0))
        self.assertEqual(r.direction, -1)


if __name__ == "__main__":
    unittest.main()
