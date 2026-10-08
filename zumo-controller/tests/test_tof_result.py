import importlib.util
from pathlib import Path
import unittest
import test_uart_control as fixtures
from test_automatic_drive import RangeSensor

spec = importlib.util.spec_from_file_location('tof_driver', Path(__file__).resolve().parents[1] / 'device-library/zumo_2040_robot/vl53l4cd.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class ToFTests(unittest.TestCase):
    def test_distance_and_status_are_one_register_block(self):
        sensor = driver.VL53L4CD.__new__(driver.VL53L4CD)
        calls = []
        def read(register, count):
            calls.append((register,count))
            return bytes([9]+[0]*12+[1,94])
        sensor._read = read
        self.assertEqual(sensor.read_result(), (350,0))
        self.assertEqual(calls, [(0x89,15)])
        sensor._read = lambda r,n: bytes([4]+[0]*12+[0,45])
        self.assertEqual(sensor.read_result(), (45,2))

    def test_status_exposes_invalid_raw_result_and_real_sample_age(self):
        f = fixtures.UARTControlTests(); f.setUp(); f.calibrate()
        sensor = RangeSensor()
        sensor.read_result = lambda: (350,0)
        f.control.tof = sensor
        f.advance(20)
        sensor.data_ready=False
        f.advance(100)
        status=f.command('2 STATUS')
        self.assertIn('DIST=350 RANGE=1', status)
        self.assertIn('RA=115 RS=0 RAW=350', status)
        sensor.read_result=lambda: (45,2)
        sensor.data_ready=True; f.advance(20)
        status=f.command('3 STATUS')
        self.assertIn('DIST=-1 RANGE=0', status)
        self.assertIn('RS=2 RAW=45', status)


if __name__ == '__main__':
    unittest.main()
