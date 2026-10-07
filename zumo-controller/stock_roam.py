"""Stock Pololu Zumo 2040: OLED eyes and tabletop roaming, no ToF.

Run from the factory C-button program menu. A calibrates/starts; C stops.
Calibrate with all five bottom sensors over the same flat tabletop.
2500 is motor PWM (out of 6000), not a measured ground speed.
Dark/transparent surfaces and blind rear edges can defeat cliff detection.
Physically validate edge recovery on the actual tabletop under supervision.
"""
import random
import sys
import time

DRIVE_SPEED = 2500
TURN_SPEED = 1500
REVERSE_SPEED = 1100
REVERSE_LIMIT_MS = 350  # Rear has no cliff sensors: keep recovery short.
CLEAR_BACKUP_MS = 85
CRUISE_MIN_MS = 800
CRUISE_MAX_MS = 1800
# Timed pivots: small, roughly quarter-turn, roughly half-turn. Actual angles
# depend on batteries, traction and motor gearing; these are not gyro targets.
RANDOM_TURN_MS = (250, 350, 650, 650, 1250)


def thresholds_for(samples):
    thresholds = []
    for i in range(5):
        values = [row[i] for row in samples]
        baseline = sum(values) // len(values)
        if baseline > 700 or max(values) - min(values) > 120:
            raise ValueError("Bad surface")
        thresholds.append(min(900, baseline + max(120, (1024 - baseline) // 3)))
    return thresholds


def edge_mask(readings, thresholds):
    if len(readings) != 5 or any(v < 0 or v > 1024 for v in readings):
        raise ValueError("Sensor error")
    return [readings[i] >= thresholds[i] for i in range(5)]


class Roamer:
    """Nonblocking motion state machine; caller samples sensors every cycle."""
    def __init__(self, now):
        self.state = "cruise"
        self.until = time.ticks_add(now, random.randint(CRUISE_MIN_MS, CRUISE_MAX_MS))
        self.clear_since = None
        self.direction = 1
        self.recoveries = 0

    def step(self, now, edge):
        danger = any(edge)
        if self.state == "failed":
            return 0, 0
        if danger and self.state != "reverse":
            # Stop on this exact sample, before commanding any recovery.
            # If the pivot meets the edge again, back up and try away again.
            # Bound consecutive unsuccessful recoveries, not normal encounters.
            self.recoveries += 1
            if self.recoveries > 3:
                self.state = "failed"
                return 0, 0
            left = sum(edge[:2])
            right = sum(edge[3:])
            self.direction = 1 if left > right else -1 if right > left else random.choice((-1, 1))
            self.state = "reverse"
            self.until = time.ticks_add(now, REVERSE_LIMIT_MS)
            self.clear_since = None
            return 0, 0
        if self.state == "reverse":
            if danger:
                self.clear_since = None
            elif self.clear_since is None:
                self.clear_since = now
            if self.clear_since is not None and time.ticks_diff(now, self.clear_since) >= CLEAR_BACKUP_MS:
                self.state = "edge_turn"
                self.until = time.ticks_add(now, random.randint(650, 1000))
                return 0, 0
            if time.ticks_diff(now, self.until) >= 0:
                self.state = "failed"
                return 0, 0
            return -REVERSE_SPEED, -REVERSE_SPEED
        if time.ticks_diff(now, self.until) >= 0:
            if self.state == "cruise":
                self.state = "turn"
                self.direction = random.choice((-1, 1))
                self.until = time.ticks_add(now, random.choice(RANDOM_TURN_MS))
            else:
                self.state = "cruise"
                self.recoveries = 0
                self.until = time.ticks_add(now, random.randint(CRUISE_MIN_MS, CRUISE_MAX_MS))
        if self.state in ("turn", "edge_turn"):
            return TURN_SPEED * self.direction, -TURN_SPEED * self.direction
        return DRIVE_SPEED, DRIVE_SPEED


def eyes(display, now, direction=0, alert=False):
    # Digit's two rounded capsule eyes, with periodic blinking and turn gaze.
    display.fill(0)
    blink = now % 3900 < 110
    height = 4 if blink else 38 if alert else 32
    width = 28
    top = (64 - height) // 2
    radius = min(7, height // 2)
    for x in (28 + direction * 5, 73 + direction * 5):
        display.fill_rect(x + radius, top, width - 2 * radius, height, 1)
        display.fill_rect(x, top + radius, width, max(1, height - 2 * radius), 1)
        for y in range(radius):
            inset = radius - int((radius * radius - (radius - y - 1) ** 2) ** 0.5)
            display.hline(x + inset, top + y, width - 2 * inset, 1)
            display.hline(x + inset, top + height - 1 - y, width - 2 * inset, 1)
    display.show()


def message(display, *lines):
    display.fill(0)
    for i, line in enumerate(lines):
        display.text(line, 0, i * 12, 1)
    display.show()


def main():
    from zumo_2040_robot import robot
    motors = robot.Motors()
    motors.off()
    try:
        display = robot.Display()  # Factory SH1106 OLED, no Digit LCD imports.
        sensors = robot.LineSensors()
        button_a, button_c = robot.ButtonA(), robot.ButtonC()
        message(display, "DIGIT ROAM", "On flat table", "A: start/cal", "C: exit")
        print("DIGIT ROAM ready: A calibrates/starts; C exits")
        while True:
            if button_c.is_pressed():
                return
            if button_a.check() is not True:
                time.sleep_ms(5)
                continue
            motors.off()
            message(display, "Calibrating...", "Keep still")
            samples = []
            for _ in range(60):
                if button_c.is_pressed():
                    return
                samples.append(list(sensors.read()))
                time.sleep_ms(10)
            try:
                thresholds = thresholds_for(samples)
            except ValueError:
                message(display, "Bad surface", "Move to flat", "bright surface", "A: retry C:exit")
                continue
            print("Stock roam thresholds:", thresholds)
            # Release A before starting; C remains responsive throughout.
            while button_a.is_pressed():
                if button_c.is_pressed():
                    return
                time.sleep_ms(5)
            now = time.ticks_ms()
            roam = Roamer(now)
            last_frame = time.ticks_add(now, -100)
            while True:
                if button_c.is_pressed():
                    motors.off()
                    message(display, "Stopped", "A: start/cal", "C: exit")
                    break
                now = time.ticks_ms()
                edge = edge_mask(sensors.read(), thresholds)
                left, right = roam.step(now, edge)
                motors.set_speeds(left, right)
                if roam.state == "failed":
                    message(display, "Edge stop", "Move to center", "A: start/cal", "C: exit")
                    break
                if time.ticks_diff(now, last_frame) >= 100:
                    eyes(display, now, roam.direction if "turn" in roam.state else 0, any(edge))
                    last_frame = now
                time.sleep_ms(2)
            # Consume releases so a held start button cannot restart motion.
            while button_a.is_pressed() or button_c.is_pressed():
                button_a.check()
                time.sleep_ms(5)
            button_a.check()
    finally:
        motors.off()


# Pololu's factory program menu launches files with __import__, not exec.
# Start on board imports as well, while keeping desktop logic tests inert.
if __name__ == "__main__" or sys.platform == "rp2":
    main()
