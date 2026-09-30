"""The same expressions scaled for the original 128x64 OLED."""

from digit_display import Display
from digit_face_engine import FaceEngine


class DigitFace(FaceEngine):
    CANVAS_WIDTH = 128
    CANVAS_HEIGHT = 64

    def __init__(self, display=None):
        super().__init__(Display() if display is None else display)
