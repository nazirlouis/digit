"""Minimal MicroPython driver for the ST VL53L4CD distance sensor.

Adapted from the Adafruit CircuitPython VL53L4CD driver.
SPDX-FileCopyrightText: 2022 Carter Nelson for Adafruit Industries
SPDX-License-Identifier: MIT
"""

import struct
import time


class VL53L4CD:
    """Read distances from one VL53L4CD on a MicroPython I2C bus."""

    DEFAULT_ADDRESS = 0x29

    _FIRMWARE_SYSTEM_STATUS = 0x00E5
    _GPIO_HV_MUX_CTRL = 0x0030
    _GPIO_TIO_HV_STATUS = 0x0031
    _SYSTEM_INTERRUPT_CLEAR = 0x0086
    _SYSTEM_START = 0x0087
    _RESULT_RANGE_STATUS = 0x0089
    _RESULT_DISTANCE = 0x0096
    _MODEL_ID = 0x010F
    _RANGE_CONFIG_A = 0x005E
    _RANGE_CONFIG_B = 0x0061

    # ST's required default configuration, covering registers 0x002D-0x0087.
    _DEFAULT_CONFIGURATION = bytes((
        0x12, 0x00, 0x00, 0x11, 0x02, 0x00, 0x02, 0x08,
        0x00, 0x08, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00,
        0x00, 0xFF, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x20, 0x0B, 0x00, 0x00, 0x02, 0x14, 0x21,
        0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC8,
        0x00, 0x00, 0x38, 0xFF, 0x01, 0x00, 0x08, 0x00,
        0x00, 0x01, 0xCC, 0x07, 0x01, 0xF1, 0x05, 0x00,
        0xA0, 0x00, 0x80, 0x08, 0x38, 0x00, 0x00, 0x00,
        0x00, 0x0F, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x07, 0x05, 0x06, 0x06, 0x00,
        0x00, 0x02, 0xC7, 0xFF, 0x9B, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00,
    ))

    # Convert the sensor's raw range-status value to ST's compact status.
    # A converted status of 0 means that the distance is valid.
    _STATUS_MAP = (
        255, 255, 255, 5, 2, 4, 1, 7,
        3, 0, 255, 255, 9, 255, 255, 255,
        255, 255, 10, 6, 255, 255, 11, 12,
    )

    def __init__(self, i2c, address=DEFAULT_ADDRESS):
        self.i2c = i2c
        self.address = address
        self._ranging = False

        model_id, module_type = self.model_info
        if model_id != 0xEB or module_type != 0xAA:
            raise RuntimeError(
                "Unexpected sensor ID: 0x{:02X} 0x{:02X}".format(
                    model_id, module_type
                )
            )

        self._initialize()

    def _write(self, register, data):
        if isinstance(data, int):
            data = bytes((data,))
        address_bytes = bytes((register >> 8, register & 0xFF))
        self.i2c.writeto(self.address, address_bytes + data)

    def _read(self, register, length=1):
        address_bytes = bytes((register >> 8, register & 0xFF))
        self.i2c.writeto(self.address, address_bytes, False)
        return self.i2c.readfrom(self.address, length)

    def _read_u16(self, register):
        return struct.unpack(">H", self._read(register, 2))[0]

    def _wait_for(self, condition, error_message, timeout_ms=1000):
        start = time.ticks_ms()
        while not condition():
            if time.ticks_diff(time.ticks_ms(), start) >= timeout_ms:
                raise RuntimeError(error_message)
            time.sleep_ms(1)

    def _initialize(self):
        self._wait_for(
            lambda: self._read(self._FIRMWARE_SYSTEM_STATUS)[0] == 0x03,
            "VL53L4CD did not finish booting",
        )
        self._write(0x002D, self._DEFAULT_CONFIGURATION)

        # Run the sensor's initial VHV calibration measurement.
        self._write(self._SYSTEM_START, 0x21)
        self._wait_for(
            lambda: self.data_ready,
            "VL53L4CD calibration timed out",
        )
        self.clear_interrupt()
        self.stop_ranging()

        self._write(0x0008, 0x09)
        self._write(0x000B, 0x00)
        self._write(0x0024, b"\x05\x00")
        self._write(0x006C, b"\x00\x00\x00\x00")
        self.set_timing_budget(50)

    @property
    def model_info(self):
        data = self._read(self._MODEL_ID, 2)
        return data[0], data[1]

    @property
    def data_ready(self):
        interrupt_status = self._read(self._GPIO_TIO_HV_STATUS)[0] & 0x01
        mux = self._read(self._GPIO_HV_MUX_CTRL)[0]
        interrupt_polarity = 0 if (mux & 0x10) else 1
        return interrupt_status == interrupt_polarity

    @property
    def distance_mm(self):
        return self._read_u16(self._RESULT_DISTANCE)

    def read_result(self):
        """Read status and distance in one transaction before clearing interrupt."""
        data = self._read(self._RESULT_RANGE_STATUS, 15)
        raw = data[0] & 0x1F
        status = self._STATUS_MAP[raw] if raw < len(self._STATUS_MAP) else 255
        return (data[13] << 8) | data[14], status

    @property
    def range_status(self):
        raw_status = self._read(self._RESULT_RANGE_STATUS)[0] & 0x1F
        if raw_status < len(self._STATUS_MAP):
            return self._STATUS_MAP[raw_status]
        return 255

    def set_timing_budget(self, budget_ms):
        """Set measurement time from 10 to 200 ms (50 ms is a good default)."""
        if self._ranging:
            raise RuntimeError("Stop ranging before changing the timing budget")
        if not 10 <= budget_ms <= 200:
            raise ValueError("Timing budget must be between 10 and 200 ms")

        osc_frequency = self._read_u16(0x0006)
        if osc_frequency == 0:
            raise RuntimeError("VL53L4CD oscillator frequency is zero")

        timing_budget_us = budget_ms * 1000 - 2500
        macro_period_us = int(2304 * (1073741824.0 / osc_frequency)) >> 6

        self._write(
            self._RANGE_CONFIG_A,
            self._encode_timeout(timing_budget_us, macro_period_us * 16),
        )
        self._write(
            self._RANGE_CONFIG_B,
            self._encode_timeout(timing_budget_us, macro_period_us * 12),
        )

    @staticmethod
    def _encode_timeout(timing_budget_us, macro_period_us):
        timing_budget_us <<= 12
        period = macro_period_us >> 6
        value = int(((timing_budget_us + (period >> 1)) / period) - 1)
        exponent = 0
        while value >> 8:
            value >>= 1
            exponent += 1
        return struct.pack(">H", (exponent << 8) | (value & 0xFF))

    def start_ranging(self):
        """Start continuous measurements and wait for the first result."""
        self._write(self._SYSTEM_START, 0x21)
        self._wait_for(
            lambda: self.data_ready,
            "VL53L4CD timed out waiting for its first measurement",
        )
        self.clear_interrupt()
        self._ranging = True

    def stop_ranging(self):
        self._write(self._SYSTEM_START, 0x00)
        self._ranging = False

    def clear_interrupt(self):
        self._write(self._SYSTEM_INTERRUPT_CLEAR, 0x01)
