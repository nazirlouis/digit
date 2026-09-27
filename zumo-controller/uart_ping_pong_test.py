"""Run directly in Thonny for 30 seconds. No motor movement commands.

Requires LCD CS on GP7 and the disabled buzzer library already installed.
Zumo GP29 RX <- XIAO D2/GPIO3 TX; Zumo GP28 TX -> XIAO D3/GPIO4 RX; shared GND.
"""
from machine import UART, Pin
import time


def run():
    uart = UART(0, baudrate=115200, bits=8, parity=None, stop=1,
                tx=Pin(28), rx=Pin(29), timeout=0, rxbuf=512)
    print("UART TEST: GP28 TX -> D3; GP29 RX <- D2; 115200 8N1")
    print("UART TEST: 30 seconds; no motor movement commands")
    buffer = bytearray()
    dropping = False
    sequence = 0
    expected = None
    sent = answered = received_pings = bad_lines = 0
    started = time.ticks_ms()
    last_ping = time.ticks_add(started, -2000)
    try:
        while time.ticks_diff(time.ticks_ms(), started) < 30000:
            data = uart.read(128)
            if data:
                for char in data:
                    if char == 13:
                        continue
                    if char == 10:
                        if not dropping and buffer:
                            line = bytes(buffer)
                            print("RX:", line)
                            if line.startswith(b"PING X "):
                                reply = b"PONG X " + line[7:]
                                uart.write(reply + b"\n")
                                received_pings += 1
                                print("TX:", reply)
                            elif expected is not None and line == expected:
                                answered += 1
                                expected = None
                                print("PASS: Zumo -> XIAO -> Zumo round trip")
                            else:
                                bad_lines += 1
                        buffer = bytearray()
                        dropping = False
                    elif not dropping:
                        if len(buffer) < 63:
                            buffer.append(char)
                        else:
                            dropping = True
                            bad_lines += 1
                            print("RX: overlong line discarded")
            now = time.ticks_ms()
            if time.ticks_diff(now, last_ping) >= 2000:
                if expected is not None:
                    print("WAIT: no matching XIAO reply yet")
                last_ping = now
                sequence += 1
                message = "PING Z {}".format(sequence).encode()
                expected = "PONG Z {}".format(sequence).encode()
                uart.write(message + b"\n")
                sent += 1
                print("TX:", message)
            time.sleep_ms(5)
    finally:
        uart.deinit()
        print("RESULT: sent={}, round_trips={}, XIAO_pings={}, other_lines={}".format(
            sent, answered, received_pings, bad_lines))
        if answered and received_pings:
            print("PASS: traffic observed in both directions")
        else:
            print("INCOMPLETE: check both TX/RX wires, shared GND, and XIAO firmware")


if __name__ == "__main__":
    run()
