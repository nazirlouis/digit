"""Silent Pololu Buzzer replacement: GP7 now belongs to the LCD's CS wire.

Install as /zumo_2040_robot/buzzer.py, then restart the Zumo. This module must
never configure a Pin, PWM, or timer. The original is in factory-backup/.
"""


class Buzzer:
    def __init__(self):
        self.volumes = []
        self.durations = []
        self.frequencies = []
        self.beats = []
        self.notes = []

    def is_playing(self):
        return False

    def set_callback(self, callback):
        # No notes play, so no playback callbacks are dispatched.
        pass

    def beep(self):
        pass

    def on(self):
        pass

    def off(self):
        pass

    def play(self, notes):
        pass

    def play_in_background(self, music):
        pass
