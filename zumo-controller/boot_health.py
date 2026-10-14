"""Remember repeated watchdog resets without ever persisting motor commands."""
import json
import os

# RP2040 watchdog register map and pico-sdk watchdog_enable/reboot contract:
# https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2040/hardware_regs/include/hardware/regs/watchdog.h
# https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/hardware_watchdog/watchdog.c
# MicroPython RP2 reset_cause conflates timer expiry and machine.reset(); use
# TIMER plus the SDK's non-reboot signature on the actual RP2040 instead.
WATCHDOG_REASON = 0x40058008
WATCHDOG_SCRATCH4 = 0x4005801c
WATCHDOG_TIMER = 1
WATCHDOG_NON_REBOOT_MAGIC = 0x6ab73121


def clear_watchdog_marker(registers):
    """After motors off/WDT disable, mark a clean exit without changing WDT state."""
    if registers[WATCHDOG_SCRATCH4] == WATCHDOG_NON_REBOOT_MAGIC:
        registers[WATCHDOG_SCRATCH4] = 0


class BootHealth:
    HEALTHY_MS = 60000

    def __init__(self, reset_cause, watchdog_cause, clock, path="watchdog_health.json", registers=None):
        self.clock, self.path = clock, path
        self.started = clock.ticks_ms()
        self.count = 0
        try:
            with open(path) as source:
                saved = json.load(source)
            if not isinstance(saved, dict) or saved.get("version") != 1:
                raise ValueError("Invalid watchdog health record")
            count = saved.get("count")
            if type(count) is not int or not 0 <= count <= 2:
                raise ValueError("Invalid watchdog reset count")
            self.count = count
        except OSError as error:
            if error.args[0] != 2:
                raise
        timed_out = (watchdog_cause is not None and reset_cause == watchdog_cause) if registers is None else (
            registers[WATCHDOG_REASON] == WATCHDOG_TIMER and
            registers[WATCHDOG_SCRATCH4] == WATCHDOG_NON_REBOOT_MAGIC)
        if timed_out:
            self.count = min(2, self.count + 1)
            self._save()
        if registers is not None:
            # Consume once: REASON is sticky across MicroPython soft resets.
            # WDT() will restore the SDK signature when supervision starts;
            # main's clean-exit path clears it before a maintenance soft reset.
            clear_watchdog_marker(registers)

    @property
    def locked(self):
        return self.count >= 2

    def _save(self):
        staged = self.path + ".new"
        with open(staged, "w") as target:
            json.dump({"version": 1, "count": self.count}, target)
        getattr(os, "replace", os.rename)(staged, self.path)

    def clear_after_user_calibration(self):
        if self.count:
            # Persist first; a write failure must not clear the runtime latch.
            previous = self.count
            self.count = 0
            try:
                self._save()
            except Exception:
                self.count = previous
                raise

    def healthy_tick(self, now, ready):
        # One isolated reset can age out after a full minute of healthy control.
        # Repeated resets never clear themselves, including on a power cycle.
        if self.count == 1:
            if not ready:
                self.started = now
            elif self.clock.ticks_diff(now, self.started) >= self.HEALTHY_MS:
                self.clear_after_user_calibration()
