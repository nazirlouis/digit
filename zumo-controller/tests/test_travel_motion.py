import unittest
from test_motion_jobs import MotionTests
from distance_calibration import validate


class Encoders:
    def __init__(self):
        self.counts = [0, 0]
    def get_counts(self):
        return tuple(self.counts)


class TravelTests(MotionTests):
    def setUp(self):
        super().setUp()
        self.feedback.encoders = Encoders()
        self.job.distance_scale = (10, 10)  # Test measurement only, never installed.

    def travel(self, mm=100):
        self.send(1, "TRAVEL", "{} 3000 6000 15000".format(mm))
        self.step(600)
        self.assertEqual(self.job.state, "RUNNING")

    def test_uncalibrated_and_invalid_bounds_never_start(self):
        self.job.distance_scale = None
        self.assertIn("DISTANCE_CALIBRATION", self.send(1, "TRAVEL", "100 3000 6000 15000"))
        self.assertEqual(self.motors.speeds, (0, 0))
        self.job.distance_scale = (10, 10)
        for mm in (0, 301, -101):
            self.assertIn("ARGUMENT", self.send(1, "TRAVEL", "{} 3000 6000 15000".format(mm)))
        self.assertEqual(self.job.seq, 0)

    def test_forward_reverse_and_rest_confirmation(self):
        for mm in (100, -100):
            self.setUp(); self.travel(mm)
            self.feedback.velocity = [200, 200] if mm > 0 else [-200, -200]
            self.feedback.encoders.counts = [mm*10, mm*10]
            self.step(10)
            self.assertEqual(self.job.state, "BRAKING")
            self.step(500)
            self.assertNotEqual(self.job.state, "DONE")
            self.feedback.velocity = [0, 0]; self.step(170)
            self.assertEqual(self.job.reason, "TRAVEL_DONE")
            self.assertEqual(self.job.travelled_mm, mm)
            self.assertIn("TRAVEL={}".format(mm), self.uart.outgoing[-1])

    def test_encoder_wrap_and_retry_keep_origin(self):
        self.feedback.encoders.counts = [0x7ffffff0,0x7ffffff0]
        self.travel(); started=self.job.started; origin=self.job.travel_counts
        self.send(1,"TRAVEL","100 3000 6000 15000")
        self.assertEqual((self.job.started,self.job.travel_counts),(started,origin))
        self.feedback.encoders.counts = [-0x7ffffc28,-0x7ffffc28]
        self.step(10); self.assertAlmostEqual(self.job.travelled_mm,100)

    def test_wrong_direction_track_mismatch_overshoot_and_io(self):
        for counts,reason in (([-100,-100],"ENCODER_DIRECTION"),([500,100],"TRACK_MISMATCH"),([1200,1200],"TRAVEL_OVERSHOOT")):
            self.setUp(); self.travel(); self.feedback.encoders.counts=counts; self.step(10)
            self.assertEqual(self.job.reason,reason); self.assertEqual(self.motors.speeds,(0,0))
        self.setUp(); self.travel(); self.feedback.encoders.get_counts=lambda: (_ for _ in ()).throw(OSError())
        self.step(10); self.assertEqual(self.job.reason,"FEEDBACK_IO")

    def test_obstacle_stale_range_stop_and_timeout(self):
        self.travel(); self.tof.distance_mm=49; self.step(25)
        self.assertEqual(self.job.reason,"OBSTACLE"); self.assertEqual(self.motors.speeds,(0,0))
        self.setUp(); self.travel(); self.tof.data_ready=False; self.step(280)
        self.assertEqual(self.job.reason,"RANGE_UNAVAILABLE")
        self.setUp(); self.travel(); self.send(0,"CANCEL"); self.step(20)
        self.assertEqual(self.motors.speeds,(0,0)); self.assertFalse(self.job.armed)

    def test_bounded_silent_expression_during_job_does_not_cancel_motion(self):
        self.control.face.cooperative=True
        self.control.face.update=lambda **kwargs: None
        self.travel()
        self.uart.send("22 SCENE HAPPY NONE 2500"); self.control.tick()
        self.assertEqual(self.job.state,"RUNNING")
        self.assertEqual(self.control.face.hold_ms,2500)
        self.uart.send("23 SCENE HAPPY NONE 10001"); self.control.tick()
        self.assertIn("ERR ARGUMENT",self.uart.outgoing[-1])

    def test_distance_calibration_is_measured_separate_and_strict(self):
        for malformed in (None, [], "invalid", 1):
            with self.assertRaises(ValueError):
                validate(malformed)
        self.assertEqual(validate({"version":1,"measured_mm":200,"left_counts":2000,"right_counts":2020}),(10,10.1))
        for mm,left,right in ((0,100,100),(99,100,100),(200,-100,100),(200,100,200),(float("nan"),100,100)):
            with self.assertRaises(ValueError): validate({"version":1,"measured_mm":mm,"left_counts":left,"right_counts":right})

if __name__ == "__main__": unittest.main()
