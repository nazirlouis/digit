"""Backport the upstream bounded synchronous NACK wait into a build-local copy.

The installed ESP-IDF tree is never changed. A changed SDK must be reviewed:
exactly one copy of the known 5.5.0 block must match, otherwise configuration
fails. The replacement follows Espressif's upstream timeout/FSM-reset fix:
https://github.com/espressif/esp-idf/blob/master/components/esp_driver_i2c/i2c_master.c
Verified 2026-10-07, synchronous I2C_EVENT_NACK branch (upstream lines 560-568).
Original/replacement driver code remains Espressif Apache-2.0 licensed.
"""

import argparse
from pathlib import Path


OLD = '''            // So wait the whole transaction finishes, then quit the function.
            while (i2c_ll_is_bus_busy(hal->dev)) {
                __asm__ __volatile__("nop");
            }
'''

NEW = '''            // So wait the whole transaction finishes, then quit the function.
            TickType_t start_tick = xTaskGetTickCount();
            const TickType_t timeout_ticks = ticks_to_wait;
            while (i2c_ll_is_bus_busy(hal->dev)) {
                if ((xTaskGetTickCount() - start_tick) > timeout_ticks) {
                    ESP_LOGE(TAG, "I2C bus is still busy but software timeout detected");
                    atomic_store(&i2c_master->status, I2C_STATUS_TIMEOUT);
                    s_i2c_hw_fsm_reset(i2c_master, true);
                    break;
                }
            }
'''


def patch_source(source: str) -> str:
    count = source.count(OLD)
    if count != 1:
        raise ValueError(
            f"Expected one ESP-IDF 5.5.0 NACK busy-wait block; found {count}. "
            "Review the SDK change and update/remove this backport explicitly."
        )
    return source.replace(OLD, NEW, 1)


def generate(source: Path, destination: Path) -> None:
    if source.resolve() == destination.resolve():
        raise ValueError("Refusing to modify the installed ESP-IDF source")
    patched = patch_source(source.read_text(encoding="utf-8"))
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Do not invalidate the object on every identical CMake configuration.
    if not destination.exists() or destination.read_text(encoding="utf-8") != patched:
        destination.write_text(patched, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    generate(args.source, args.destination)
    print("Digit: bounded I2C NACK timeout applied to build-local driver copy")
