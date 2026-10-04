"""Digit hardware configuration."""

# Informational compatibility value; active display and face imports use the LCD.
DISPLAY_TYPE = "lcd"

# Positive gyro Z should result from motors (-speed, +speed).
# Change to -1 ONLY if a physical test reports LOOK_DIRECTION with reversed yaw.
VOICE_TURN_MOTOR_SIGN = 1
