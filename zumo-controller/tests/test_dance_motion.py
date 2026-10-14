"""Dance through the real framed job parser, visibility and motor safety loop."""
import unittest
import test_motion_jobs as jobs
from test_expression_motion import GestureFace
from dance_motion import DURATION_MS, STEPS


class DanceTests(unittest.TestCase):
    send = jobs.MotionTests.send
    step = jobs.MotionTests.step

    def setUp(self):
        jobs.MotionTests.setUp(self)
        self.face = GestureFace(self.clock)
        self.control.face = self.control.dance_show.face = self.face
        self.control.wake.face = self.face
        self.outputs = []
        original = self.motors.set_speeds
        def record(left, right):
            self.outputs.append((left, right))
            original(left, right)
        self.motors.set_speeds = record

    def start(self, reveal=True):
        self.send(1, "DANCE", "8000")
        self.assertEqual(self.job.kind, "DANCE")
        if reveal:
            self.face.reveal()
            self.step(5)

    def stopped(self):
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.control.dance_show.started)
        self.assertIsNone(self.control.dance_motion.sequence)

    def test_visible_happy_before_motion_and_finite_completion(self):
        saved = [(self.control.rgb_leds.get(i), self.control.rgb_leds.get_brightness(i)) for i in range(6)]
        self.start(False)
        self.step(100)
        self.assertEqual(self.motors.speeds, (0, 0))
        self.assertIsNone(self.control.dance_show.started)
        self.face.reveal()
        self.step(5)
        self.assertEqual(self.motors.speeds, (-3000, 3000))
        self.assertEqual(self.job.reason, "DANCING")
        self.assertEqual(self.face.expression, "happy")
        self.step(5200)
        self.assertEqual((self.job.state, self.job.reason), ("DONE", "DANCE_DONE"))
        self.assertFalse(self.job.position_known)
        self.stopped()
        self.assertEqual(saved, [(self.control.rgb_leds.get(i), self.control.rgb_leds.get_brightness(i)) for i in range(6)])
        self.assertEqual(sum(step[2] for step in STEPS), DURATION_MS)
        for left, right in self.outputs:
            self.assertIn((left, right), ((0, 0), (-3000, 3000), (3000, -3000)))
        previous = (0, 0)
        for output in self.outputs:
            if output != previous:
                if previous != (0, 0) and output != (0, 0):
                    self.fail("Reversal without a stopped pause")
                previous = output

    def test_duplicate_job_never_restarts_dance(self):
        self.start()
        self.step(200)
        started = self.control.dance_motion.started
        self.send(1, "DANCE", "8000")
        self.assertEqual(self.control.dance_motion.started, started)
        self.step(5100)
        count = len(self.outputs)
        self.send(1, "DANCE", "8000")
        self.step(50)
        self.assertEqual(len(self.outputs), count)
        self.stopped()

    def test_stop_lease_and_loop_gap_cancel_all_decorations(self):
        for reason in ("stop", "lease", "gap"):
            self.setUp()
            self.start()
            if reason == "stop":
                self.uart.send("5 STOP")
                self.control.tick()
            elif reason == "lease":
                self.step(800, heartbeat=False)
            else:
                self.clock.now += 51
                self.control.tick()
            self.stopped()
            self.assertFalse(self.job.active)
            self.step(100)
            self.stopped()

    def test_obstacle_edge_encoder_and_scene_replacement_stop(self):
        for fault in ("obstacle", "edge", "encoder", "scene"):
            self.setUp()
            self.start()
            if fault == "obstacle":
                self.tof.distance_mm = 30
            elif fault == "edge":
                self.sensors.values = [1000] * 5
            elif fault == "encoder":
                self.feedback.fail = True
            else:
                self.face.show_scene("confused", "NONE")
            self.step(250)
            self.assertEqual(self.motors.speeds, (0, 0), fault)
            self.stopped()
            self.assertFalse(self.job.active)
            self.assertFalse(self.job.recovery.active)

    def test_unseen_face_and_invalid_duration_never_move(self):
        self.send(1, "DANCE", "30000")
        self.assertIsNone(self.job.kind)
        self.start(False)
        self.step(1600)
        self.stopped()
        self.assertTrue(all(output == (0, 0) for output in self.outputs))


if __name__ == "__main__":
    unittest.main()
