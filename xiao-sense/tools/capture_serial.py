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
    # Set the USB control lines before opening so attaching diagnostics does
    # not request an ESP reset or clear the voice-session counters.
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    with connection:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = connection.readline()
            if line:
                sys.stdout.write(line.decode("utf-8", errors="replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
