"""Simulated unequal tracks exercise velocity feedback, saturation and stops."""
import unittest
import test_uart_control as fixtures
Clock = fixtures.Clock
from encoder_speed import EncoderSpeed

CAL = dict(version=1, low_cps=[1500, 1200], high_cps=[4000, 3600],
           reverse_low_cps=[1550, 1350], reverse_high_cps=[4100, 3700])


class Encoders:
    counts = (0, 0)
    fail = False
    def get_counts(self):
        if self.fail:
            raise OSError("disconnected")
        return tuple(int(v) for v in self.counts)


class EncoderTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.encoder = Encoders()
        self.loop = EncoderSpeed(self.encoder, self.clock, CAL)

    def test_both_tracks_reach_same_speed_under_unequal_load(self):
        for direction in (1, -1):
            self.setUp()
            outputs = [0, 0]
            for n in range(140):
                # Slow the right track by 8% to require feedback beyond feedforward.
                rates = []
                for i in range(2):
                    low = CAL['low_cps' if direction > 0 else 'reverse_low_cps'][i]
                    high = CAL['high_cps' if direction > 0 else 'reverse_high_cps'][i]
                    rate = max(0, low+(abs(outputs[i])-3000)*(high-low)/3000) if outputs[i] else 0
                    rates.append(rate * direction * (1 if i == 0 else .92))
                self.encoder.counts = tuple(self.encoder.counts[i]+rates[i]*.05 for i in range(2))
                self.clock.now += 50
                outputs = self.loop.update(self.clock.now, (direction*4000,)*2, outputs)
            self.assertLess(abs(abs(rates[0])-abs(rates[1])), 50)
            self.assertLess(abs(abs(rates[0])-abs(self.loop.targets[0])), 50)
            self.assertTrue(all(3000 <= abs(v) <= 6000 for v in outputs))
            self.assertEqual(self.loop.update(self.clock.now, (0,0), outputs, True), (0,0))
            self.assertEqual(self.loop.integral, [0,0])

    def test_pivot_has_equal_opposite_velocity_targets(self):
        outputs = self.loop.update(0, (-3000,3000), (0,0))
        self.assertEqual(self.loop.targets[0], -self.loop.targets[1])
        self.assertLess(outputs[0], 0)
        self.assertGreater(outputs[1], 0)
        self.assertNotEqual(abs(outputs[0]), abs(outputs[1]))

    def test_orbit_ratio_tracks_measured_velocity_at_all_dashboard_speeds(self):
        for speed in (3000, 4000, 6000):
            for inside in (0, 1):
                with self.subTest(speed=speed, inside=inside):
                    self.setUp()
                    ratios = (1/3, 1) if inside == 0 else (1, 1/3)
                    outputs = [0, 0]
                    for _ in range(240):
                        rates = [max(0, CAL['low_cps'][i] + (outputs[i]-3000) *
                                     (CAL['high_cps'][i]-CAL['low_cps'][i])/3000) *
                                 (1 if i == 0 else .92) if outputs[i] else 0 for i in range(2)]
                        self.encoder.counts = tuple(self.encoder.counts[i]+rates[i]*.05 for i in range(2))
                        self.clock.now += 50
                        outputs = self.loop.update(self.clock.now, (speed, speed), outputs, rate_ratios=ratios)
                    self.assertGreater(min(rates), 0)
                    self.assertLessEqual(self.loop.targets[inside] / self.loop.targets[1-inside], 1/3 + .001)
                    self.assertLess(abs(rates[inside] / rates[1-inside] - 1/3), .025)
                    self.assertLess(outputs[inside], outputs[1-inside])
                    # A normal target immediately drops the circle ratio.
                    self.loop.update(self.clock.now, (4000, 4000), outputs)
                    self.assertEqual(self.loop.targets[0], self.loop.targets[1])

    def test_inside_track_stall_is_detected_even_at_low_power(self):
        self.loop.update(50, (4000, 4000), (3000, 3000), rate_ratios=(1/3, 1))
        with self.assertRaisesRegex(RuntimeError, "ENCODER_STALL"):
            for n in range(1, 18):
                self.encoder.counts = (0, n*100)
                self.loop.update(50+n*50, (4000, 4000), (2000, 4000), rate_ratios=(1/3, 1))

    def test_zero_and_reversal_reset_integrator(self):
        self.loop.update(0, (4000,4000), (4000,4000))
        self.loop.integral = [500,500]
        self.loop.update(0, (-4000,0), (4000,4000))
        self.assertEqual(self.loop.integral, [0,0])
        self.assertEqual(self.loop.targets[1], 0)

    def test_stall_wrong_direction_stale_and_io_fail_closed(self):
        for failure in ('stall','direction','stale','io'):
            with self.subTest(failure=failure):
                self.setUp()
                with self.assertRaises(RuntimeError):
                    for n in range(25):
                        self.clock.now += 300 if failure == 'stale' else 50
                        if failure == 'direction':
                            self.encoder.counts = (-10*n, -10*n)
                        self.encoder.fail = failure == 'io'
                        self.loop.update(self.clock.now, (3000,3000), (3000,3000))

    def test_counter_wrap_is_not_motion_spike(self):
        self.loop.counts = (2147483640,)*2
        self.encoder.counts = (-2147483646,)*2
        self.loop.update(50, (3000,3000), (3000,3000))
        self.assertEqual(self.loop.velocity, [100,100])

    def test_bad_calibration_rejected(self):
        for change in ({'version':0}, {'low_cps':[0,1]}, {'high_cps':[1500,1500]}, {'low_cps':[float('nan'),1000]}):
            with self.assertRaises(ValueError):
                EncoderSpeed(self.encoder, self.clock, dict(CAL, **change))

    def test_controller_ramps_and_stop_cannot_restart_from_feedback(self):
        f = fixtures.UARTControlTests(); f.setUp(); f.calibrate()
        f.control.speed_control = EncoderSpeed(self.encoder, f.clock, CAL)
        f.command('2 DRIVE 3000 3000 6000')
        f.advance(100)
        self.assertEqual(f.motors.speeds, (600,600))
        f.command('3 STOP'); f.advance(1000)
        self.assertEqual(f.motors.speeds, (0,0))
        f.command('4 DRIVE 3000 3000 6000')
        self.encoder.fail = True; f.advance(50)
        self.assertEqual(f.motors.speeds, (0,0))
        self.assertEqual(f.control.fault, 'ENCODER')
        self.assertFalse(f.control.ready)


if __name__ == '__main__':
    unittest.main()
