import json
from pathlib import Path
import tempfile
import unittest

from test_uart_control import Clock
from boot_health import (BootHealth, clear_watchdog_marker, WATCHDOG_REASON,
                         WATCHDOG_SCRATCH4, WATCHDOG_NON_REBOOT_MAGIC)


class BootHealthTests(unittest.TestCase):
    def setUp(self):
        folder = Path(__file__).resolve().parents[1] / "test-artifacts" / "boot-health"
        folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.addCleanup(self.temp.cleanup)
        self.path = str(Path(self.temp.name) / "health.json")
        self.clock = Clock()

    def boot(self, cause=0):
        return BootHealth(cause, 3, self.clock, self.path)

    def test_clean_boot_does_not_write_or_lock(self):
        health = self.boot()
        self.assertFalse(health.locked)
        self.assertFalse(Path(self.path).exists())

    def test_repeat_latches_across_soft_reset_and_power_cycle(self):
        self.assertFalse(self.boot(3).locked)
        self.assertEqual(self.boot(0).count, 1)
        self.assertTrue(self.boot(3).locked)
        health = self.boot(0)
        health.healthy_tick(120000, True)
        self.assertTrue(health.locked)
        self.assertTrue(self.boot().locked)
        health.clear_after_user_calibration()
        self.assertFalse(self.boot().locked)

    def test_single_watchdog_ages_out_only_after_continuous_health(self):
        health = self.boot(3)
        health.healthy_tick(30000, False)
        health.healthy_tick(60000, True)
        self.assertEqual(health.count, 1)
        health.healthy_tick(90000, True)
        self.assertEqual(health.count, 0)
        self.assertEqual(self.boot().count, 0)

    def test_corrupt_record_cannot_silently_unlock(self):
        for value in ("not json", '{"version":1,"count":-1}', '{"version":1,"count":true}'):
            Path(self.path).write_text(value)
            with self.assertRaises(ValueError):
                self.boot()

    def test_failed_clear_keeps_lock(self):
        self.boot(3)
        health = self.boot(3)
        def fail():
            raise OSError("write failed")
        health._save = fail
        with self.assertRaises(OSError):
            health.clear_after_user_calibration()
        self.assertTrue(health.locked)
        self.assertTrue(self.boot().locked)

    def rp2_boot(self, registers):
        return BootHealth(3, 3, self.clock, self.path, registers=registers)

    def test_rp2_real_timer_fault_is_consumed_once_before_wdt_rearming(self):
        registers = {WATCHDOG_REASON: 1, WATCHDOG_SCRATCH4: WATCHDOG_NON_REBOOT_MAGIC}
        self.assertEqual(self.rp2_boot(registers).count, 1)
        self.assertEqual(registers[WATCHDOG_SCRATCH4], 0)
        self.assertEqual(self.rp2_boot(registers).count, 1)
        # A new WDT enable and actual unclean timeout restores the SDK marker.
        registers[WATCHDOG_SCRATCH4] = WATCHDOG_NON_REBOOT_MAGIC
        self.assertTrue(self.rp2_boot(registers).locked)

    def test_rp2_clean_ctrl_c_ctrl_d_does_not_recount_sticky_reason(self):
        registers = {WATCHDOG_REASON: 1, WATCHDOG_SCRATCH4: WATCHDOG_NON_REBOOT_MAGIC}
        self.assertEqual(self.rp2_boot(registers).count, 1)
        registers[WATCHDOG_SCRATCH4] = WATCHDOG_NON_REBOOT_MAGIC  # WDT constructor.
        clear_watchdog_marker(registers)  # Main finally after off/disable.
        self.assertEqual(self.rp2_boot(registers).count, 1)
        self.assertFalse(self.rp2_boot(registers).locked)

    def test_rp2_intentional_and_power_resets_never_count_as_watchdog_faults(self):
        for reason, signature in ((0, 0), (2, 0), (1, 0), (2, WATCHDOG_NON_REBOOT_MAGIC),
                                  (1, 0xb007c0d3)):
            registers = {WATCHDOG_REASON: reason, WATCHDOG_SCRATCH4: signature}
            self.assertEqual(self.rp2_boot(registers).count, 0)
            self.assertFalse(Path(self.path).exists())

    def test_clean_exit_does_not_overwrite_sdk_bootloader_signature(self):
        registers = {WATCHDOG_REASON: 2, WATCHDOG_SCRATCH4: 0xb007c0d3}
        clear_watchdog_marker(registers)
        self.assertEqual(registers[WATCHDOG_SCRATCH4], 0xb007c0d3)


if __name__ == "__main__":
    unittest.main()
