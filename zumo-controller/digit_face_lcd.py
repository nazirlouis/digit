"""Expression-colored animated eyes for Digit's 1.69-inch LCD."""

from digit_display import Display
from digit_face_engine import FaceEngine


class DigitFace(FaceEngine):
    ORANGE = 0xFCE0
    BACKGROUND = 0x0000

    def __init__(self, display=None):
        if display is None:
            display = Display(width=self.CANVAS_WIDTH, height=self.CANVAS_HEIGHT,
                              foreground=self.ORANGE, background=self.BACKGROUND)
        super().__init__(display)
