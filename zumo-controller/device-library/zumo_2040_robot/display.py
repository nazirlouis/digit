"""Route legacy Pololu display imports to Digit's Waveshare LCD.

Install as /zumo_2040_robot/display.py with /digit_display_lcd.py.
This also covers robot.Display and the factory splash/menu without patching.
"""

from digit_display_lcd import Display
