"""Choose the attached screen, then restart the Zumo."""

# "oled" = original Pololu SH1106; "lcd" = Waveshare 1.69-inch ST7789V2.
DISPLAY_TYPE = "lcd"

# Positive gyro Z should result from motors (-speed, +speed).
# Change to -1 ONLY if a physical test reports LOOK_DIRECTION with reversed yaw.
VOICE_TURN_MOTOR_SIGN = 1
