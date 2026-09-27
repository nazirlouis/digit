"""Select the face layout matching the attached screen."""

from digit_config import DISPLAY_TYPE

if DISPLAY_TYPE == "oled":
    from digit_face_oled import DigitFace
elif DISPLAY_TYPE == "lcd":
    from digit_face_lcd import DigitFace
else:
    raise ValueError("DISPLAY_TYPE must be 'oled' or 'lcd'")
