"""Firmware recovery through actual UART/M2 parsers, independent sensor inputs."""
import unittest
import test_edge_retreat as fixtures
from edge_reverse import SETTLE_MS, SETTLE_LIMIT_MS


class SampledImu:
    """Independent sensor updates; reading consumes only that sensor's sample."""
    def __init__(self, clock, field, value, period_ms, delay_ms=0):
        self.clock, self.field = clock, field
        self.value, self.period_ms = value, period_ms
        self.first_at = clock.ticks_ms() + delay_ms
        self.enabled = True
        self.consumed = -1
        self.read_times = []
        setattr(self, field, value)

    def data_ready(self):
        elapsed = self.clock.ticks_ms() - self.first_at
        return self.enabled and elapsed >= 0 and elapsed // self.period_ms > self.consumed

    def read(self):
        if not self.data_ready():
            raise AssertionError("read before an independent sample was ready")
        now = self.clock.ticks_ms()
        self.consumed = (now - self.first_at) // self.period_ms
        setattr(self, self.field, self.value)
        self.read_times.append(now)


class EdgeReverseTests(unittest.TestCase):
    def setUp(self):
        self.f = fixtures.EdgeRetreatTests()
        self.f.setUp()
        self.c = self.f.control
        self.thresholds = self.c.thresholds[:]
        # Exercise the explicit fallback independently of automatic recovery.
        self.f.forward(opt_in=False)
        self.f.sensors.values = [1024, 100, 100, 100, 100]
        self.c.last_line_readings = self.f.sensors.values[:]
        self.c._latch("EDGE")
        self.f.feedback.velocity = [0, 0]
        self.assertFalse(self.c.motion.armed)
        self.assertTrue(self.c.edge_reverse.available())
        self.assertEqual(self.f.motors.speeds, (0, 0))

    def manual(self):
        self.f.uart.send("30 DRIVE -6000 -6000 6000")
        self.c.tick()
        return self.f.uart.outgoing[-1]

    def step(self, ms, manual=False, heartbeat=True):
        for i in range(ms//5):
            if manual and i % 20 == 0:
                self.manual()
            self.f.step(5, heartbeat=heartbeat)

    def back_job(self):
        self.f.session += 1
        self.assertNotIn(" ERR ", self.f.send(0, "ARM"))
        self.assertNotIn(" ERR ", self.f.send(1, "BACK", "100"))
        self.step(100)
        self.assertEqual(self.c.edge_reverse.state, "BACK")
        self.assertLess(self.f.motors.speeds[0], 0)

    def start_reverse(self, manual=False):
        if manual:
            self.assertIn("OK DRIVE", self.manual())
        else:
            self.f.session += 1
            self.assertNotIn(" ERR ", self.f.send(0, "ARM"))
            self.assertNotIn(" ERR ", self.f.send(1, "BACK", "100"))

    def finish(self, manual=False, distance=100):
        self.f.feedback.encoders.counts = [400-distance*10]*2
        self.f.feedback.velocity = [-350, -343]
        self.step(10, manual)
        self.assertEqual(self.c.edge_reverse.state, "SETTLE")
        self.assertEqual(self.f.motors.speeds, (0, 0))
        self.f.feedback.velocity = [0, 0]
        self.step(250, manual)
        self.assertTrue(self.c.ready)
        self.assertEqual(self.c.edge_reverse.state, "DONE")

    def test_manual_reverse_immediately_applies_3000_and_ignores_imu(self):
        self.f.gyro.last_reading_dps = (30, 0, 0)
        self.f.acc.last_reading_g = (.985, 0, .174)
        self.assertIn("OK DRIVE", self.manual())
        self.assertLessEqual(self.f.motors.speeds[0], -3000)
        self.step(50, manual=True)
        self.f.feedback.encoders.counts = [330, 330]
        self.step(20, manual=True)
        self.assertEqual(self.c.edge_reverse.back_mm, 7)
        self.assertTrue(self.c.edge_reverse.active)
        self.finish(manual=True)
        self.manual(); self.step(100, manual=True)
        self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_m2_back_completes_without_imu_floor_clearance_or_range(self):
        self.c.voice_turn = None
        self.c.accelerometer = self.c.edge_retreat.acc = None
        self.start_reverse()
        self.assertLessEqual(self.f.motors.speeds[0], -3000)
        self.f.sensors.values = [1024]*5
        self.f.tof.data_ready = False
        self.finish()
        self.assertEqual(self.c.motion.reason, "EDGE_BACK_DONE")
        self.assertEqual(self.c.motion.travelled_mm, -100)
        self.assertTrue(any(" DONE EDGE_BACK_DONE " in line and "TRAVEL=-100" in line
                            for line in self.f.uart.outgoing))

    def test_short_requested_back_still_has_its_own_encoder_distance_cap(self):
        self.f.session += 1
        self.f.send(0, "ARM")
        self.assertNotIn(" ERR ", self.f.send(1, "BACK", "20"))
        self.finish(distance=20)
        self.assertEqual(self.c.motion.travelled_mm, -20)

    def test_reverse_session_cannot_turn_drive_or_replay_forward(self):
        self.f.session += 1
        self.f.send(0, "ARM")
        for verb, args in (("TRAVEL", "-20 3000 6000 15000"), ("TURN", "30 6000 10000"), ("DRIVE", "3000 3000 6000 50 10000")):
            self.assertIn("EDGE_REVERSE_ONLY", self.f.send(1, verb, args))
        self.assertIn("SESSION", self.f.send(1, "TRAVEL", "100 3000 6000 15000", session=1))
        self.f.send(1, "BACK", "100")
        self.finish()
        self.assertIn("EDGE_REVERSE_ONLY", self.f.send(2, "TURN", "30 6000 10000"))

    def test_stale_sensor_and_forward_coast_do_not_abort_reverse(self):
        self.start_reverse()
        self.f.gyro.ready = self.f.acc.ready = False
        self.f.feedback.encoders.counts = [429, 427]
        self.f.feedback.velocity = [1864, 1856]
        self.step(25)
        self.assertTrue(self.c.edge_reverse.active)
        self.assertLessEqual(self.f.motors.speeds[0], -3000)
        self.finish()

    def test_stop_or_lease_loss_prevents_any_later_reverse_replay(self):
        for fault in ("stop", "heartbeat", "manual_stop", "manual_lease"):
            with self.subTest(fault=fault):
                self.setUp(); manual=fault.startswith("manual")
                self.start_reverse(manual)
                if fault.endswith("stop"):
                    self.f.uart.send("30 STOP");self.c.tick()
                else:
                    self.step(760 if fault=="heartbeat" else 310, heartbeat=False)
                self.assertFalse(self.c.edge_reverse.active)
                self.assertEqual(self.f.motors.speeds, (0, 0))
                self.f.sensors.values=[100]*5
                self.step(1500, heartbeat=False)
                self.assertFalse(self.c.ready)
                self.assertFalse(self.c.edge_reverse.available())

    def test_button_encoder_fault_and_original_deadline_stop_recovery(self):
        for fault in ("button", "encoder", "deadline"):
            with self.subTest(fault=fault):
                self.setUp(); self.start_reverse()
                if fault=="button": self.f.c.pressed=True
                if fault=="encoder": self.f.feedback.fail=True
                if fault=="deadline": self.c.edge_reverse.started=self.f.clock.now-10000
                self.step(10)
                self.assertFalse(self.c.edge_reverse.active)
                self.assertEqual(self.f.motors.speeds, (0, 0))

    def test_directional_manual_requests_remain_locked_under_edge(self):
        for cmd in ("DRIVE 3000 3000", "DRIVE -3000 3000", "DRIVE -3000 -4000", "TURN 3000", "SAFE 3000 3000"):
            self.f.uart.send("30 "+cmd);self.c.tick()
            self.assertIn(" ERR ", self.f.uart.outgoing[-1])
            self.assertEqual(self.f.motors.speeds, (0, 0))


if __name__ == "__main__":
    unittest.main()
