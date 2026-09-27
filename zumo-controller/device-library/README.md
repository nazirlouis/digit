# Digit device library replacements

The original Pololu library is preserved in `../factory-backup/`. The
files here are replacements for the corresponding files in the Zumo's existing
`/zumo_2040_robot/` directory:

- `zumo_2040_robot/motors.py` reverses the **left motor** so a positive speed
  commands forward motion on both tracks. The right motor is unchanged.
- `zumo_2040_robot/encoders.py` restores the **factory encoder signs**. The
  lifted-track test showed that inverting the left encoder made its forward
  count negative. Both forward counts should now be positive.
- `zumo_2040_robot/buzzer.py` disables the buzzer without claiming any pins.
  Required with LCD CS on **GP7** and the **Buzzer = 7** jumper disconnected.
  Install it as `/zumo_2040_robot/buzzer.py`, then restart. Existing startup
  sounds and buzzer calls become silent. The original is preserved in
  `../factory-backup/zumo_2040_robot/buzzer.py` for a future wiring rollback.

In Thonny, stop the running program and replace the Zumo's
`/zumo_2040_robot/encoders.py` with the file here. Keep the already installed
`motors.py` patch; if it is not installed yet, copy that file too. Reset the
Zumo so MicroPython reloads the library. This makes the correction apply to
`motor_test.py`, `move_forward.py`, `spin.py`, and `autonomous_drive.py` without
editing each program. Do not copy the rest of the factory backup over the
installed library.

For a physical check, keep both tracks lifted and run
`../motor_polarity_check.py` directly in Thonny. It waits three seconds,
then runs the left and right motors separately at speed 1000 for 500 ms each.
Each tested track should turn forward and its corresponding count should be
positive. Stop the program immediately if a track moves unexpectedly. The
script stops both motors when interrupted. Physical behavior cannot be
established by a desktop syntax check.
