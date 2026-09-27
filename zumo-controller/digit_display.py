"""Select the screen in digit_config.py; both drivers remain available."""

from digit_config import DISPLAY_TYPE

if DISPLAY_TYPE == "oled":
    from zumo_2040_robot.display import Display
elif DISPLAY_TYPE == "lcd":
    from digit_display_lcd import Display
else:
    raise ValueError("DISPLAY_TYPE must be 'oled' or 'lcd'")
