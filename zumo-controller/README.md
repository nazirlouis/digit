# Digit Zumo controller — Waveshare LCD

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

- `digit_config.py` — selects `DISPLAY_TYPE = "lcd"` (change to `"oled"` to switch back).
- `digit_display.py` and `digit_face.py` — select the matching screen and face.
- `digit_display_lcd.py` and `digit_face_lcd.py` — LCD driver and face.
- `main_menu.py` — original six-second splash/program menu, adapted to the LCD.
- `main.py` and `uart_control.py` — default face and UART manual control;
  A for sad, B for happy, C for local motor stop.
- `move_forward.py` and `spin.py` — use the new display through `DigitFace`.
- `autonomous_drive.py` — roaming and calibration/status screens.
- `tof_distance.py` — standalone distance display.
- `display_test.py` — manual screen test, without motor commands.

Keep the existing `/zumo_2040_robot` library on the device. As before, save
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
Its factory demos still target the original OLED; do not copy that snapshot over
the updated Digit programs. The XIAO project handles audio/camera/web controls
and contains no physical SPI screen driver.

## Layout and configuration

Normal factory screens retain their centered 128×64 canvas for compatibility.
Digit's face uses a higher-resolution 136×112 canvas that expands to 272×224,
leaving only a four-pixel side margin and eight-pixel top/bottom margin. Each
logical pixel becomes a 2×2 square. The face palette is orange (`0xFCE0`) on
black; other screens still default to white on black. `set_colors(foreground,
background)` changes the logical monochrome palette.

The driver stores a 1 KB logical framebuffer, a 1 KB previous frame, a 512-byte
transfer row, and about 8 KB of pixel lookup data (plus Python object overhead).
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

After the screen test, check distance readings with `tof_distance.py`. Validate
movement programs with tracks lifted first: display transfers add latency, so
desktop checks cannot establish physical obstacle/edge response timing.

## XIAO dashboard command link

The default `main.py` listens on Zumo UART0: GP29 RX from XIAO D2/GPIO3 TX,
GP28 TX to XIAO D3/GPIO4 RX, plus a shared ground. Both sides use 3.3 V UART,
115200 baud, 8 data bits, no parity, and 1 stop bit. This requires LCD CS on
GP7, with the buzzer jumper disconnected and the silent buzzer library in place.
The XIAO dashboard sends repeated `STATUS` requests. The Zumo answers whenever
they arrive, so either board can finish booting first; no startup handshake or
wait delay is required.

Save `uart_control.py` and the updated `main.py` to the Zumo device root. Keep
the existing `digit_face.py`, `digit_config.py`, matching face/display files,
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
| `17 STOP` | Stop immediately; also cancels calibration |
| `18 FACE HAPPY` or `19 FACE SAD` | Play the expression while stopped |

Calibrate with the robot stationary on safe floor. It samples the five downward
sensors over about 600 ms and rejects an unsuitable baseline. `DRIVE` is
rejected until `0 EVENT READY`. Resend `DRIVE` every 100 ms while a dashboard
control is held, and send `STOP` when it is released. If no valid `DRIVE`
arrives for 300 ms, the Zumo stops and sends `0 EVENT TIMEOUT`; `PING` and
`STATUS` do not keep it moving. An edge reading or button C stops both tracks,
sends `0 EVENT EDGE` or `0 EVENT BUTTON_C`, and locks movement until the robot
is repositioned and `CALIBRATE` succeeds again. A line-sensor error similarly
locks movement with `0 EVENT SENSOR`. `STATUS` reports the current condition;
for example, `13 OK READY=1 FAULT=NONE L=0 R=0 BAT=7600 EDGE=0`. Front ToF
obstacle stopping and remote autonomous roaming are not part of manual mode.

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
