# Digit Zumo controller — Waveshare LCD

The voice-command upgrade uses the updated `main.py` and `uart_control.py` in
this folder. It adds front-distance reporting and guarded `SAFE` drive commands
for the XIAO's routines. See [voice actions and installation](../xiao-sense/VOICE_ACTIONS.md).
`STOP` cancels the active turn/wake effect and resets the face to neutral.
The standalone `autonomous_drive.py` remains a separate program; dashboard
**Explore** now runs through the UART controller with edge-stop locking.

The optional LCD is the [Waveshare 1.69-inch LCD module](https://www.waveshare.com/product/displays/lcd-oled/lcd-oled-3/1.69inch-lcd-module.htm)
([purchased screen](https://www.amazon.com/dp/B0C238JCLH)): ST7789V2, 240×280, SPI.
The driver displays it in landscape (280×240).

## Wiring

Power off before changing wiring. Connect by signal name, not cable position:
the Waveshare connector does not have the same pin order as the old OLED.

| LCD pin | Wire | Zumo connection |
| --- | --- | --- |
| VCC | Purple | 3V3 |
| GND | White | GND |
| DIN | Green | GP3 / display MOSI |
| CLK | Orange | GP2 / display SCK |
| CS | Yellow | GP7, with the "Buzzer = 7" jumper disconnected |
| DC | Blue | GP0 / display D/C |
| RST | Brown | GP1 / display RESET |
| BL | Gray | **Connect to 3V3** for always-on backlight |

Move the LCD's CS wire from GP28 to GP7 after disconnecting the "Buzzer = 7"
jumper. Install `device-library/zumo_2040_robot/buzzer.py` as
`/zumo_2040_robot/buzzer.py` and restart: the original buzzer library claims GP7
even when imported without playing a sound. The replacement keeps existing menu
and demo calls silent and never touches GP7. GP28/GP29 are now available for UART.
BL needs 3V3 for a visible backlight. GP4/GP5 remain on the ToF and IMU I2C bus.
See [Pololu pin assignments](https://www.pololu.com/docs/0J87/6.10).

GP0 is shared with button C, and SPI0/GP3 are shared with the RGB LEDs. The new
driver selects CS only during transfers and detaches GP2 between transfers,
following the original Pololu driver's clock-sharing approach. Do not drive the
display and LEDs simultaneously from interrupts or another core.

## Install on the Zumo

In Thonny, stop the running program, then save these files to the MicroPython
device root, keeping their names:

- `digit_config.py` records the LCD hardware configuration.
- `digit_display.py` and `digit_face.py` route all active programs to the LCD.
- `digit_face_engine.py` — shared expression shapes and animation timing; required for both screens.
- `digit_display_lcd.py` and `digit_face_lcd.py` — LCD driver and face.
- `digit_face_oled.py` is retained only as historical source; active programs do not import it.
- `main_menu.py` — original six-second splash/program menu, adapted to the LCD.
- `main.py` and `uart_control.py` — default face and UART manual control;
  A for sad, B for happy, C for local motor stop.
- `wake_reaction.py` — timed wake LED effect and happy face, required by `uart_control.py`.
- `move_forward.py` and `spin.py` — use the new display through `DigitFace`.
- `autonomous_drive.py` — roaming and calibration/status screens.
- `tof_distance.py` — standalone distance display.
- `display_test.py` — manual screen test, without motor commands.
- `face_test.py` — all-expression gallery; stops the motors before starting.

For the button, encoder, IMU, motor, and full self-tests, also upload
`button_test.py`, `encoder_test.py`, `imu_test.py`, `motor_test.py`, and
`self_test.py` from this folder to the device root, replacing the old versions.
They import `digit_display.Display` directly and use the LCD, including when
launched directly in Thonny.

Upload the remaining demos from this folder too: `proximity_demo.py`,
`ir_sensor_demo.py`, `line_follower.py`, `face_opponent.py`, `face_uphill.py`,
`gyro_turn.py`, `rotation_resist.py`, `i2c0_scan.py`, `rgb_demo.py`, `sys_info.py`,
`error.py`, `music.py`, `siren.py`, and `blink.py`. Sensor and movement logic
is unchanged; the siren retains its LED animation with audio disabled for GP7.

Install `device-library/zumo_2040_robot/display.py` as
`/zumo_2040_robot/display.py`, alongside `/digit_display_lcd.py` at the device
root. This routes **all legacy `robot.Display()` and library display imports**
to the LCD, including the factory splash/menu and older installed demos.
Restart the Zumo to clear cached imports.

Keep the rest of the existing `/zumo_2040_robot` library on the device. As before, save
the silent `device-library/zumo_2040_robot/buzzer.py` inside that package (not
the device root). Restart after installing it to clear the old buzzer/PWM state.
Save
`vl53l4cd.py` inside `/zumo_2040_robot/` for the distance and roaming programs.
For Digit's rewired left motor, replace the library's `motors.py` with the
version in `device-library/zumo_2040_robot/`. If you previously installed the
left encoder inversion, also restore `encoders.py` from that directory;
see `device-library/README.md` for the installation and lifted-track check.
Upload the driver before restarting any updated program. No new firmware or
third-party MicroPython package is required; use the Zumo MicroPython build
that already supports the face's `framebuf.ellipse()` calls.

At startup, A exits to the Thonny REPL, B enters the UF2 bootloader, and C opens
the original program/debug-file menu. If no button is pressed for six seconds,
`main.py` starts automatically. Newer Pololu firmware runs `main_menu.py` as the
launcher, so the menu must live there rather than inside `main.py`; otherwise
the startup song and splash run twice. The menu temporarily redirects the
factory library's `Display` class to `digit_display.Display`, allowing the
factory splash, menu, and debug scripts to draw on the new LCD.

`factory-backup/` is the original restoration snapshot and remains unchanged.
Use the updated test files in this folder, not the archived tests in that snapshot.
Its factory demos still target the original OLED; do not copy that snapshot over
the updated Digit programs. The XIAO project handles audio/camera/web controls
and contains no physical SPI screen driver.

## Layout and configuration

Active programs always use the LCD; changing `DISPLAY_TYPE` does not enable
the archived OLED path. `factory-backup/` remains the original restoration copy.

Normal factory screens retain their centered 128×64 canvas for compatibility.
Digit's face uses a higher-resolution 136×112 canvas that expands to 272×224,
leaving only a four-pixel side margin and eight-pixel top/bottom margin. Each
logical pixel becomes a 2×2 square. Idle eyes are orange (`0xFCE0`) on black;
expressions use the colors below. Other screens still default to white on
black. `set_colors(foreground, background)` changes the logical monochrome
palette. A palette change resends the canvas even if its eye shapes are unchanged.

The driver stores a logical framebuffer and a previous frame (1,024 bytes each
for 128×64, or 1,904 bytes each for the 136×112 face), a reusable transfer buffer
(1,024 or 1,088 bytes), and 256 bytes of pixel lookup data plus object overhead.
The lookup uses 16 mutable entries, each expanding four monochrome pixels.
Changing colors updates those same entries in place; it does not create a
second lookup table or a 256-entry tuple. This removes the color-change memory
spike in the original driver, which could exhaust the Zumo's MicroPython heap.
It sends only the byte-aligned rectangle containing changed pixels and combines
each pair of scaled rows into one SPI write instead of allocating a 134 KB
full-color framebuffer. A full 136×112 face transfer contains 121,856 pixel-data
bytes, while normal eye motion transfers only its changed bounding rectangle.
Real frame time must still be measured on the robot.

If the image is upside down, change `FLIP_180 = True` in `digit_display_lcd.py` and
restart. The default SPI clock is 20 MHz; try 10 MHz if the image corrupts with
long wires. The initialization includes the module's 20-pixel landscape column
offset, RGB565 format, and panel inversion.

## Check the installed screen

If the backlight is on but the panel stays solid black, run `lcd_raw_test.py`
first. It bypasses the framebuffer and face code, slows SPI to 1 MHz, fills the
whole panel with red/green/blue/white, and tries SPI modes 0 and 3. Watch both
the panel and Thonny Shell. If neither mode shows a color but the Shell reaches
`finished`, the program is running and the next check is the five control/data
wires: CS=GP7, DC=GP0, RST=GP1, DIN=GP3, and CLK=GP2. If mode 0 works, keep
the normal driver on mode 0 and lower `SPI_BAUDRATE`; if only mode 3 works,
change the normal driver's `polarity` and `phase` to 1.

Run `display_test.py` manually with Digit stationary. It sends red, green, blue,
white, and black fills, followed by an orientation/border pattern and the face.
Press A and B to check sad/happy transitions; C exits. The first RGB LED toggles
once a second to exercise the shared bus. Console logs include frame-transfer
time and free memory. Inspect the image for clipping, swapped colors, flicker,
and rotation. Driver startup logs confirm commands were sent, not that the
write-only display responded.

`motor_test.py` and `self_test.py` can move the motors: lift both tracks first.
The factory `self_test.py` also drives GP28/GP29 during its pin checks; disconnect
the external XIAO UART wiring before running it. The UART and motor-only
diagnostics print to the serial console and do not initialize a screen.

After the screen test, check distance readings with `tof_distance.py`. Validate
movement programs with tracks lifted first: display transfers add latency, so
desktop checks cannot establish physical obstacle/edge response timing.

## XIAO dashboard command link

### Face expressions

The face supports **neutral, happy, sad, mad, serious, curious, surprised,
sleeping, confused, sleepy, excited, crying, and love (heart eyes)**. Dizzy
has been removed from the active face code and demos. Sad now uses connected,
drooping eyes with no tear; crying adds alternating falling tears. Happy has
rounded smile arcs and gentle motion. New selections transition from the
currently displayed eyes, including when interrupting sleep or another reaction.
Sleeping holds until another expression is selected. Other reactions return to
neutral automatically; neutral/wake up restores normal looking and blinking.

The LCD palette is defined once in `COLORS` near the top of
`digit_face_engine.py`:

| Expression | Eye color |
| --- | --- |
| Neutral | Digit orange |
| Happy | Warm gold |
| Sad / crying | Soft blue / deeper blue |
| Mad | Red |
| Serious | Silver |
| Curious | Turquoise |
| Surprised | One brief white flash, then cyan |
| Sleeping / sleepy | Dim indigo / muted lavender |
| Confused | Violet |
| Excited | Bright yellow |
| Love | Pink |

Surprise flashes the eyes for a target 160 ms once per selection; the background
stays black. `SURPRISE_FLASH_COLOR` and `SURPRISE_FLASH_MS` control that effect.
An interrupted flash is canceled by the new expression. Colors return to orange
when the expression finishes or neutral is selected. Sleeping keeps its dim
color until replaced. The original OLED still shows the same shapes in monochrome.
The small palette is updated in place only when the eye color changes, not on
every animation frame. Check the flash timing and brightness on the physical LCD.

If the expression update is already installed, this color update requires only
`digit_face_engine.py`, `digit_face_lcd.py`, and the memory-efficient
`digit_display_lcd.py` on the Zumo, followed by a restart.
No XIAO firmware change is needed for colors.

If selecting a face reports `MemoryError` with the earlier color driver,
replace `digit_display_lcd.py` on the Zumo and restart before retrying. The new
lookup's pixel storage is 256 bytes instead of 8,192 bytes and is reused across
all colors. If an error remains, copy the complete Thonny traceback (filenames
and line numbers included) to identify the failing allocation.

For this update, save `digit_face_engine.py`, `digit_face_lcd.py`,
`digit_face_oled.py`, `digit_display_lcd.py`, `uart_control.py`, `face_test.py`, `spin.py`, and
`move_forward.py` to the device root in Thonny. Keep the existing
`digit_face.py`, `digit_display.py`, `digit_config.py`,
and robot library. Restart so the updated modules are loaded.

Run `face_test.py` directly in Thonny: **A = next expression, B = replay,
C = exit**. The Shell prints each selected expression. This test keeps the
motors stopped and does not need the XIAO or floor calibration. The normal
`main.py` button shortcuts remain A = sad, B = happy, C = motor stop.

Build/upload the active `xiao-sense` project using environment
`xiao_esp32s3_sense_idf` for the dashboard's full expression button grid.
Update both boards to use all dashboard choices. Face commands remain rejected
while moving, and the controller skips screen updates while driving.
Selecting a face does not arm the motors or clear a safety latch.

`previews/digit-expressions.png`, `previews/digit-face-motion.gif`, and
`previews/digit-color-motion.gif` are desktop renders of the face pixels and
palette. The color animation shows mad, surprised, sleeping, and love.
Regenerate with `python tools/render_faces.py`
(Pillow required). They do not verify the physical screen's timing or appearance.

### Wake reaction update

Save **`wake_reaction.py`, `voice_turn.py`, `uart_control.py`, `main.py`, and
`digit_config.py`** in the device root, then restart. Keep the
current face/display modules installed, including `digit_face_engine.py`.
The XIAO plays the sound through the ReSpeaker speaker; this feature does not
use the Zumo buzzer or change its disconnected GP7 jumper.

`ID WAKE` starts a happy transition with a 2.2-second blue/cyan/purple LED sweep
and pulse, returning `ID OK WAKE`. It works with driving locked and never changes
motor/calibration state. Moving, calibration, an existing reaction, or a held
button C rejects the request. Missing LEDs return `ERR LED_UNAVAILABLE`.
An LED/display failure returns `ERR WAKE_IO` or `0 EVENT WAKE_FAILED`.

The effect advances on 30 ms timer ticks in the normal loop, after motor safety
and UART handling, with no animation sleeps. It restores all six prior LED
colors and brightness values. Valid manual LED/face commands, buttons A/B/C,
STOP, calibration, and accepted motion cancel the effect. Manual changes are
never overwritten by an old effect's completion. LED/display transfers stay
sequential because those peripherals share SPI0/GP3.

With the motors stopped, use the XIAO dashboard's **Test wake reaction** before
testing speech. Check the visible animation and the physical speaker: desktop
tests and protocol acknowledgements cannot verify either output.

### Voice turn update

The five files above add a gyro-controlled, one-shot rotation before a wake
reaction. No MicroPython firmware replacement is needed. The existing Pololu
IMU library stays installed. `main.py` detects and initializes it on I2C0
GP4/GP5; if unavailable, manual drive/face/light control still starts and
`STATUS` reports `VOICE=0`. Do not overwrite the current motor polarity or
silent buzzer library with factory files.

The XIAO dashboard provides **Set front**, **Set left**, and **Enable voice
turning**. See `../xiao-sense/README.md` for mounting and setup. Direction setup
does not move motors and resets when the XIAO restarts. Floor calibration is
still required. Wake listening remains on unless vision is enabled.

`ID LOOK angle` accepts integer degrees -180 through 180, positive toward the
robot's left. It replies `ID OK LOOKING`, then `ID OK LOOK_DONE` only after
stopping and settling. While active the XIAO sends `NEW_ID KEEP ID` every
100 ms; a KEEP for another/completed turn cannot start or extend motion.
The 300 ms lease covers gyro calibration as well as movement. LOOK is never
automatically retried; a duplicate active LOOK is rejected as busy.

`voice_turn.py` samples the stationary gyro bias for at least 400 ms (80
samples), then uses gyro Z feedback at speeds up to 1800. It stops within an
8-degree band, settles for 150 ms, and has an overall 6-second limit. Missing
or invalid gyro data, movement during bias measurement, reversed yaw,
or no heading progress stops the motors and reports a `LOOK_*` error followed
by `0 EVENT TURN_FAULT`. Reposition and recalibrate the floor before trying
again. Display updates are skipped throughout the turn, including gyro
calibration, so they cannot delay motor control.

STOP and valid manual DRIVE/TURN commands cancel the voice turn. Edge sensors,
Button C, sensor faults, and the UART watchdog retain priority. After a normal
completion, the XIAO sends WAKE and plays its speaker chime. On cancellation or
failure it drops that pending completion reaction.

The gyro convention matches Pololu's example: `(-speed, +speed)` produces
positive Z rotation. Physical testing is required with this robot's wiring.
`VOICE_TURN_MOTOR_SIGN` in `digit_config.py` is +1 by default; change it to -1
only after confirming physically reversed yaw (`LOOK_DIRECTION`). Do not change
encoder or motor-library polarity to compensate without checking manual drive.

### Protocol

The default `main.py` listens on Zumo UART0: GP29 RX from XIAO D2/GPIO3 TX,
GP28 TX to XIAO D3/GPIO4 RX, plus a shared ground. Both sides use 3.3 V UART,
115200 baud, 8 data bits, no parity, and 1 stop bit. This requires LCD CS on
GP7, with the buzzer jumper disconnected and the silent buzzer library in place.
The XIAO dashboard sends repeated `STATUS` requests. The Zumo answers whenever
they arrive, so either board can finish booting first; no startup handshake or
wait delay is required.

Save `uart_control.py`, `wake_reaction.py`, `voice_turn.py`, `digit_config.py`, and the updated `main.py` to the Zumo device root. Keep
the existing `digit_face.py`, matching face/display files,
and `/zumo_2040_robot` library. Reset the Zumo. Once `main.py` starts, it
prints `UART CONTROL` and says the motors are stopped. On every reset, driving
starts locked until calibration. Opening Thonny can interrupt the running
program and leave the REPL prompt; use a power cycle with Thonny disconnected
when checking automatic startup.

Send one ASCII command per line, ending with `\n` (an optional preceding `\r`
is accepted). The first field is a decimal request ID from 0 to 65535. Each
command receives a matching `ID OK ...` or `ID ERR ...` reply. Unsolicited
state changes use `0 EVENT ...`; reserve ID 0 for those events if the dashboard
needs to distinguish them. Incoming lines may contain at most 63 bytes before
the newline. Request IDs must be 1 through 65535; ID 0 is reserved for
events and uncorrelated errors. Commands are uppercase.

| Example command | Result |
| --- | --- |
| `12 PING` | `12 OK PONG` |
| `13 STATUS` | Readiness, fault, applied left/right speeds, battery millivolts, and edge latch |
| `14 CALIBRATE` | `14 OK CALIBRATING`, then `0 EVENT READY` or `0 EVENT CAL_FAILED` |
| `15 DRIVE 4000 4000` | Run both tracks forward; each speed must be -6000 through 6000 |
| `16 DRIVE -1000 1000` | Turn in place; sign and direction depend on the installed motor polarity patch |
| `20 TURN -4000` | Rotate left with equal and opposite tracks; positive speed rotates right, range -6000 to 6000 |
| `17 STOP` | Stop immediately; also cancels calibration |
| `29 BRAKE` | Cancel motion and decelerate to zero at the fixed rate; safety stops retain priority |
| `18 FACE HAPPY` or `19 FACE SAD` | Play an expression while stopped; all names listed above are supported in uppercase |
| `24 FACE SLEEPING` / `25 FACE NEUTRAL` | Stay asleep / wake up and resume idle |
| `21 LED ALL 255 0 0 5` | Set all six RGB LEDs red at brightness 5 (range 0-31) |
| `22 LED 2 0 0 255 5` | Set LED 3 blue; indices are 0-5 |
| `23 LED ALL 0 0 0 0` | Turn all RGB LEDs off |
| `26 WAKE` | Happy face and LED effect while stationary; sound is played by XIAO |
| `27 LOOK 90` | One measured left turn, then `27 OK LOOK_DONE`; requires floor calibration and gyro |
| `28 KEEP 27` | Renew only active LOOK 27 for another 300 ms |

Motion starts with a ramp on the Zumo itself. `DRIVE left right acceleration`
optionally selects 500-6000 motor speed units per second (protocol fallback
3000). The dashboard Acceleration slider starts at 6000; lower values give a
gentler start. Repeated hold commands renew the watchdog without restarting
the ramp. DRIVE, SAFE, and TURN use the ramp; gyro LOOK turns use a fixed 6000
units/second to retain their stall deadline.

Normal dashboard release, Stop, and STOP ALL cancel pending actions and send
`BRAKE`. Both tracks then slow to zero at a fixed 12000 speed units per second,
independent of the slider: about 0.33 seconds from 4000 or 0.5 seconds from 6000.
Speed reductions and direction reversals use this same fixed braking rate;
a reversed track passes through zero before accelerating the opposite way.
`STATUS` advertises `BRAKE=1`. Until the updated Zumo file is installed, the
XIAO uses the older peer's immediate STOP command for compatibility.

`STOP` remains the immediate safety command. Button C, confirmed edges, sensor
faults, lost drive keepalives, and loss of dashboard control immediately stop
and clear pending motion. Braking retains edge and automatic ToF checks and
has its own 750 ms completion deadline, so a stalled control loop cannot leave
the tracks powered indefinitely. Repeated BRAKE commands do not extend that
deadline or restart the deceleration. Slow loop acceleration/braking increments
are capped at 20 ms rather than catching up in one jump.

Floor-calibrated edge thresholds now have an extra 60 counts of margin.
Marginal readings must stay above threshold on the same sensor for 20 ms
before latching EDGE. A reading at least 950 (or the calibrated threshold,
if higher) still stops immediately. Short marginal front-end lifts can clear
without latching; persistent indications still stop and require recalibration.

Copy the updated `uart_control.py` to the Zumo device root and reset it. Use
the updated active XIAO `xiao-sense` firmware, refresh the dashboard, and
recalibrate on safe floor. With tracks lifted, check acceleration, smooth
release/Stop, and immediate Button C stopping. Then check small front lifts
and actual edge detection at low speed on a safely supported surface before
relying on the new threshold/filter settings.

Calibrate with the robot stationary on safe floor. It samples the five downward
sensors over about 600 ms and rejects an unsuitable baseline. `DRIVE` and
`TURN` are rejected until `0 EVENT READY`. Resend either motion command every
100 ms while active, and send `STOP` when it ends. If no valid motion command
arrives for 300 ms, the Zumo stops and sends `0 EVENT TIMEOUT`; `PING` and
`STATUS` do not keep it moving. An edge reading or button C stops both tracks,
sends `0 EVENT EDGE` or `0 EVENT BUTTON_C`, and locks movement until the robot
is repositioned and `CALIBRATE` succeeds again. A line-sensor error similarly
locks movement with `0 EVENT SENSOR`. `STATUS` reports the current condition;
for example, `13 OK READY=1 FAULT=NONE L=0 R=0 BAT=7600 EDGE=0 VOICE=1`. Front ToF
obstacle stopping and remote autonomous roaming are not part of manual mode.
The XIAO's optional person-turning mode uses `TURN` only; it does not request
forward or reverse travel or use ToF distance.
`LED` accepts red, green, and blue values from 0-255 and works without floor
calibration. The dashboard offers a color picker, brightness slider, individual
LED selection, and Off. The `OK LED` reply confirms the Zumo sent the color to
its LED driver; check the physical LEDs to confirm the result.

To verify on hardware, run `uart_ping_pong_test.py` with the existing temporary
XIAO test firmware first; confirm both round-trip counters are nonzero. Then
restore/use XIAO firmware that sends the commands above. With Zumo tracks
lifted, check `PING`, `STATUS`, `FACE`, and calibration before sending a low
speed `DRIVE`; confirm release/timeout and button C stop the tracks. Finally,
on safe floor, test low-speed driving and an edge trigger. Serial replies and
desktop tests cannot establish actual track direction or edge response time.

## Reference and desktop checks

Initialization values are adapted from Waveshare's `Arduino/LCD_1inch69/LCD_Driver.cpp`
in its [LCD reference archive](https://files.waveshare.com/upload/8/8d/LCD_Module_code.zip).
See [module documentation](https://www.waveshare.com/wiki/1.69inch_LCD_Module)
and `LICENSE.waveshare.txt` for the reference code license.

From this folder run `python -B -m unittest discover -s tests -v` for mocked
transport checks: offsets, pixel byte order and scaling, partial updates,
shared SPI/LED/button behavior, and recovery after transfer errors. These do
not verify the physical panel or on-device performance.
