import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "patch_i2c_timeout.py"
spec = importlib.util.spec_from_file_location("patch_i2c_timeout", SCRIPT)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class I2cTimeoutBackportTests(unittest.TestCase):
    def test_exact_replacement_retains_surrounding_driver(self):
        before = "driver before\n" + patch.OLD + "driver after\n"
        after = patch.patch_source(before)
        self.assertEqual(after, "driver before\n" + patch.NEW + "driver after\n")
        self.assertNotIn('__asm__ __volatile__("nop")', after)
        self.assertIn("xTaskGetTickCount() - start_tick", after)
        self.assertIn("timeout_ticks = ticks_to_wait", after)
        self.assertIn("I2C_STATUS_TIMEOUT", after)
        self.assertIn("s_i2c_hw_fsm_reset(i2c_master, true)", after)
        self.assertIn("break;", after)

    def test_sdk_drift_or_ambiguous_match_fails_closed(self):
        for source in ("different driver", patch.NEW, patch.OLD + patch.OLD):
            with self.assertRaises(ValueError):
                patch.patch_source(source)

    def test_original_is_immutable_and_generation_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sdk.c"
            destination = Path(directory) / "build" / "patched.c"
            source.write_bytes(patch.OLD.replace("\n", "\r\n").encode())
            original = source.read_bytes()
            patch.generate(source, destination)
            modified_at = destination.stat().st_mtime_ns
            patch.generate(source, destination)
            self.assertEqual(destination.stat().st_mtime_ns, modified_at)
            self.assertEqual(source.read_bytes(), original)
            with self.assertRaises(ValueError):
                patch.generate(source, source)


if __name__ == "__main__":
    unittest.main()
