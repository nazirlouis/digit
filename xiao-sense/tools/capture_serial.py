"""Read XIAO startup diagnostics without sending any UART commands."""

import argparse
import sys
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--seconds", type=float, default=20)
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2) as connection:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = connection.readline()
            if line:
                sys.stdout.write(line.decode("utf-8", errors="replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
