"""Digit's Waveshare 1.69-inch ST7789V2 LCD on the Zumo display header.

Drawing uses the original 128x64 monochrome coordinates. show() expands each
pixel to 2x2 RGB565 pixels, centered on a 280x240 landscape panel. This keeps
all expressions and status screens proportional without a large framebuffer.
See README.md for wiring and the manufacturer's reference sources.
"""

from machine import Pin, SPI
import framebuf
import time


# User wiring: BL must be connected to 3V3; there is no backlight GPIO.
SCK_PIN = 2
MOSI_PIN = 3
DC_PIN = 0
RESET_PIN = 1
CS_PIN = 7  # Buzzer jumper disconnected; install the disabled buzzer library.
SPI_BAUDRATE = 20_000_000
# Set True if the landscape image is upside down in the installed mount.
FLIP_180 = False


class Display(framebuf.FrameBuffer):
    width = 128
    height = 64
    panel_width = 280
    panel_height = 240

    def __init__(self, width=128, height=64, foreground=0xFFFF,
                 background=0x0000):
        if width % 8:
            raise ValueError("display width must be a multiple of 8")
        if width * 2 > self.panel_width or height * 2 > self.panel_height:
            raise ValueError("logical canvas does not fit the LCD")
        self.width = width
        self.height = height
        self.row_bytes = width // 8
        self.x_offset = (self.panel_width - width * 2) // 2
        self.y_offset = (self.panel_height - height * 2) // 2
        self.buffer = bytearray(self.row_bytes * height)
        super().__init__(self.buffer, self.width, self.height, framebuf.MONO_HLSB)
        self._previous = bytearray(len(self.buffer))
        self._valid = False
        # Cooperative mode snapshots the newest complete drawing, then scans
        # and transfers a bounded number of logical rows on each service call.
        # Never keep SPI/CS owned while returning to motor or button polling.
        self.cooperative = False
        self._queued = bytearray(len(self.buffer))
        self._row_valid = bytearray(self.height)
        self._row_foreground = [None] * self.height
        self._row_background = [None] * self.height
        self._scan_row = 0
        self._scan_remaining = 0
        self.frame_pending = False
        self.frame_id = 0
        self.completed_frame_id = 0
        self.last_slice_us = 0
        self.max_slice_us = 0
        # Two scaled physical rows are prepared together and sent in one write.
        self._rows = bytearray(self.width * 8)
        self._rows_view = memoryview(self._rows)
        # Four source pixels per lookup entry (two entries per source byte).
        # Allocate once: 16 mutable blocks use only 256 bytes of pixel data.
        # Palette changes update these blocks instead of allocating another
        # 256-entry table while the old one is still live on the RP2040 heap.
        self._lookup = tuple(bytearray(16) for _ in range(16))
        self._foreground = None
        self._background = None
        self._next_foreground = foreground
        self._next_background = background
        # Reuse command/window storage during the safety-sensitive service
        # path. Initialization and the legacy synchronous API remain simple.
        self._command_byte = bytearray(1)
        self._window_bytes = bytearray(4)
        self.last_show_us = 0
        self.last_rows_sent = 0
        self.last_bytes_sent = 0
        self.cs = Pin(CS_PIN, Pin.OUT, value=1)
        self.dc = Pin(DC_PIN, Pin.OUT, value=0)
        self.reset = Pin(RESET_PIN, Pin.OUT, value=1)
        self.sck = Pin(SCK_PIN)
        self.spi = SPI(0, baudrate=SPI_BAUDRATE, polarity=0, phase=0,
                       sck=self.sck, mosi=Pin(MOSI_PIN), miso=None)
        # Detach the display clock while idle, just like Pololu's OLED driver.
        # SPI0/MOSI are also used by the RGB LEDs, with their own clock on GP6.
        self.sck.init(Pin.OUT, value=0)
        self.set_colors(foreground, background)
        self.reset(1)
        time.sleep_ms(20)
        self.reset(0)
        time.sleep_ms(20)
        self.reset(1)
        time.sleep_ms(120)
        self._initialize()
        self.clear_panel()
        print("LCD: ST7789V2 280x240 landscape; canvas {}x{} at 2x".format(
            self.width, self.height
        ))
        print("LCD: SPI0 CLK=2 DIN=3 DC=0 RST=1 CS=7; BL needs 3V3")
        print("LCD: initialization sent (write-only; panel not detected)")

    def _begin(self):
        self.cs(1)
        self.spi.init(baudrate=SPI_BAUDRATE, polarity=0, phase=0)
        # RGBLEDs and ButtonC can reconfigure GP0 between frames.
        self.dc.init(Pin.OUT, value=0)
        self.sck.init(Pin.ALT, alt=1)
        self.cs(0)

    def _end(self):
        self.cs(1)
        self.sck.init(Pin.OUT, value=0)
        self.dc(0)

    def _command(self, command, data=None):
        self.dc(0)
        self._command_byte[0] = command
        self.spi.write(self._command_byte)
        if data is not None:
            self.dc(1)
            self.spi.write(data)

    def _initialize(self):
        # Register values from Waveshare's LCD_1inch69/LCD_Driver.cpp.
        # 0x70 is their landscape scan setting; flip both axes for the other
        # landscape orientation. The visible GRAM columns start at 20.
        settings = (
            (0x36, b"\xb0" if FLIP_180 else b"\x70"),
            (0x3A, b"\x05"),  # 16-bit RGB565 over SPI
            (0xB2, b"\x0b\x0b\x00\x33\x35"),
            (0xB7, b"\x11"), (0xBB, b"\x35"),
            (0xC0, b"\x2c"), (0xC2, b"\x01"),
            (0xC3, b"\x0d"), (0xC4, b"\x20"),
            (0xC6, b"\x13"), (0xD0, b"\xa4\xa1"),
            (0xD6, b"\xa1"),
            (0xE0, b"\xf0\x06\x0b\x0a\x09\x26\x29\x33\x41\x18\x16\x15\x29\x2d"),
            (0xE1, b"\xf0\x04\x08\x08\x07\x03\x28\x32\x40\x3b\x19\x18\x2a\x2e"),
            (0xE4, b"\x25\x00\x00"),
            (0x21, None),  # IPS panel requires inversion enabled
            (0x11, None),  # sleep out
        )
        try:
            self._begin()
            for command, data in settings:
                self._command(command, data)
        finally:
            self._end()
        time.sleep_ms(120)
        try:
            self._begin()
            self._command(0x29)  # display on
        finally:
            self._end()
        time.sleep_ms(20)

    def _window(self, x0, y0, x1, y1):
        # Inclusive endpoints in landscape; 240x280 glass in 240x320 GRAM.
        x0 += 20
        x1 += 20
        data = self._window_bytes
        data[0], data[1], data[2], data[3] = x0 >> 8, x0 & 255, x1 >> 8, x1 & 255
        self._command(0x2A, data)
        data[0], data[1], data[2], data[3] = y0 >> 8, y0 & 255, y1 >> 8, y1 & 255
        self._command(0x2B, data)
        self._command(0x2C)
        self.dc(1)

    def set_colors(self, foreground=0xFFFF, background=0x0000):
        """Change RGB565 colors in place; no new lookup buffers or tuple."""
        self._next_foreground, self._next_background = foreground, background
        if self.cooperative:
            # A palette belongs to a submitted complete frame, never to a
            # half-rasterized future frame while an older one is in flight.
            return
        self._apply_colors(foreground, background)

    def _apply_colors(self, foreground, background):
        if foreground == self._foreground and background == self._background:
            return
        self._valid = False
        if not self.cooperative:
            for row in range(self.height):
                self._row_valid[row] = 0
        fg_hi, fg_lo = foreground >> 8, foreground & 255
        bg_hi, bg_lo = background >> 8, background & 255
        # MONO_HLSB is most-significant bit first. Each source pixel expands
        # to two horizontal RGB565 pixels; show() duplicates the whole row.
        for value in range(16):
            block = self._lookup[value]
            for bit in range(4):
                foreground_pixel = value & (8 >> bit)
                hi = fg_hi if foreground_pixel else bg_hi
                lo = fg_lo if foreground_pixel else bg_lo
                start = bit * 4
                block[start] = hi
                block[start + 1] = lo
                block[start + 2] = hi
                block[start + 3] = lo
        self._foreground = foreground
        self._background = background

    def clear_panel(self, color=0x0000):
        """Clear the whole physical panel, including the canvas margins."""
        row = bytes((color >> 8, color & 255)) * self.panel_width
        self._valid = False
        for y in range(self.height):
            self._row_valid[y] = 0
        try:
            self._begin()
            self._window(0, 0, self.panel_width - 1, self.panel_height - 1)
            for _ in range(self.panel_height):
                self.spi.write(row)
        finally:
            self._end()

    def load_pbm(self, filename):
        """Load a 128x64 monochrome PBM used by the factory splash screen."""
        with open(filename, "rb") as source:
            source.readline()  # P4 magic number
            source.readline()  # creator comment
            source.readline()  # dimensions
            data = bytearray(source.read())
        return framebuf.FrameBuffer(
            data, self.width, self.height, framebuf.MONO_HLSB
        )

    def exception(self, error):
        """Show a compact exception summary on the physical display."""
        self.fill(0)
        self.text(type(error).__name__ + ":", 0, 0, 1)
        message = str(error)
        for line in range(7):
            self.text(message[line * 16:(line + 1) * 16], 0, 8 * (line + 1), 1)

    @staticmethod
    def show_exception(error):
        display = Display()
        display.exception(error)
        display.show()

    def show(self, full_update=False):
        """Send the byte-aligned rectangle containing all changed pixels."""
        if self.cooperative:
            return self.queue_frame(full_update)
        started = time.ticks_us()
        buf = self.buffer
        previous = self._previous
        row_bytes = self.row_bytes
        first_y = self.height
        last_y = -1
        first_byte = row_bytes
        last_byte = -1
        if full_update or not self._valid:
            first_y, last_y = 0, self.height - 1
            first_byte, last_byte = 0, row_bytes - 1
        else:
            for y in range(self.height):
                offset = y * row_bytes
                for x_byte in range(row_bytes):
                    index = offset + x_byte
                    if buf[index] != previous[index]:
                        if y < first_y:
                            first_y = y
                        last_y = y
                        if x_byte < first_byte:
                            first_byte = x_byte
                        if x_byte > last_byte:
                            last_byte = x_byte
        self.last_rows_sent = 0
        self.last_bytes_sent = 0
        if last_y >= first_y:
            rows = self._rows
            rows_view = self._rows_view
            lookup = self._lookup
            changed_byte_count = last_byte - first_byte + 1
            physical_row_bytes = changed_byte_count * 32
            try:
                self._begin()
                x0 = self.x_offset + first_byte * 16
                x1 = self.x_offset + (last_byte + 1) * 16 - 1
                y0 = self.y_offset + first_y * 2
                y1 = self.y_offset + (last_y + 1) * 2 - 1
                self._window(x0, y0, x1, y1)
                for y in range(first_y, last_y + 1):
                    offset = y * row_bytes + first_byte
                    for x_byte in range(changed_byte_count):
                        start = x_byte * 32
                        value = buf[offset + x_byte]
                        rows[start:start + 16] = lookup[value >> 4]
                        rows[start + 16:start + 32] = lookup[value & 15]
                    rows_view[physical_row_bytes:physical_row_bytes * 2] = \
                        rows_view[:physical_row_bytes]
                    self.spi.write(rows_view[:physical_row_bytes * 2])
                previous[:] = buf
                self._valid = True
                self.last_rows_sent = (last_y - first_y + 1) * 2
                self.last_bytes_sent = (
                    physical_row_bytes * self.last_rows_sent
                )
            except BaseException:
                # A partial transfer must be retried even if drawing later
                # returns to the previous frame's pixels.
                self._valid = False
                raise
            finally:
                self._end()
        self.last_show_us = time.ticks_diff(time.ticks_us(), started)

    def enable_cooperative(self):
        """Make show() enqueue only; callers must service flush() regularly."""
        if self.cooperative:
            return
        self.cooperative = True
        for row in range(self.height):
            self._row_valid[row] = 1 if self._valid else 0
            self._row_foreground[row] = self._foreground
            self._row_background[row] = self._background

    def queue_frame(self, full_update=False):
        """Replace pending pixels atomically with the newest complete canvas.

        The queued buffer is independent of subsequent drawing. Every row is
        compared with pixels actually sent, so replacing an unfinished frame
        cannot lose erasures. Palette changes repaint only affected pixels;
        changing eye color must not retransmit unchanged black margins.
        """
        self._queued[:] = self.buffer
        self._apply_colors(self._next_foreground, self._next_background)
        if full_update:
            for row in range(self.height):
                self._row_valid[row] = 0
        self.frame_id += 1
        if not self.frame_pending:
            self._scan_row = 0
        self.frame_pending = True
        # Keep advancing around the panel if producers replace frames faster
        # than SPI can flush them. Restarting at row zero would starve the
        # lower eyes/caption forever. One full sweep validates this revision.
        self._scan_remaining = self.height
        return self.frame_id

    def cancel_frame(self):
        """Discard pending work without claiming unsent pixels were displayed."""
        self.frame_pending = False

    def flush(self, max_rows=2, max_us=1500):
        """Service at most max_rows logical rows, releasing shared SPI always.

        max_us is checked between rows, not a hard real-time deadline: a
        single row conversion/write cannot be preempted in MicroPython.
        Even unchanged row scans count toward the work limit.
        """
        started = time.ticks_us()
        self.last_rows_sent = 0
        self.last_bytes_sent = 0
        serviced = 0
        bus_owned = False
        try:
            while self.frame_pending and serviced < max_rows:
                if serviced and time.ticks_diff(time.ticks_us(), started) >= max_us:
                    break
                y = self._scan_row
                offset = y * self.row_bytes
                first, last = self.row_bytes, -1
                foreground_changed = self._row_foreground[y] != self._foreground
                background_changed = self._row_background[y] != self._background
                for x in range(self.row_bytes):
                    previous = self._previous[offset+x]
                    if (not self._row_valid[y] or self._queued[offset+x] != previous or
                            (foreground_changed and previous != 0) or
                            (background_changed and previous != 255)):
                        if first == self.row_bytes:
                            first = x
                        last = x
                if last >= first:
                    count = last - first + 1
                    length = count * 32
                    for x in range(count):
                        value = self._queued[offset+first+x]
                        start = x * 32
                        self._rows[start:start+16] = self._lookup[value >> 4]
                        self._rows[start+16:start+32] = self._lookup[value & 15]
                    self._rows_view[length:length*2] = self._rows_view[:length]
                    try:
                        if not bus_owned:
                            # SPI reconfiguration is appreciably slower than
                            # a row transfer on RP2040. Own it for this bounded
                            # slice, not separately for each row. _end still
                            # runs before returning to safety/Button C/RGB.
                            bus_owned = True
                            self._begin()
                        self._window(self.x_offset + first*16, self.y_offset + y*2,
                                     self.x_offset + (last+1)*16-1,
                                     self.y_offset + y*2+1)
                        self.spi.write(self._rows_view[:length*2])
                    except BaseException:
                        self._valid = False
                        self._row_valid[y] = 0
                        raise
                    for x in range(first, last+1):
                        self._previous[offset+x] = self._queued[offset+x]
                    self.last_rows_sent += 2
                    self.last_bytes_sent += length*2
                self._row_valid[y] = 1
                self._row_foreground[y] = self._foreground
                self._row_background[y] = self._background
                self._scan_row = (self._scan_row + 1) % self.height
                self._scan_remaining -= 1
                serviced += 1
                if self._scan_remaining == 0:
                    self.frame_pending = False
                    self.completed_frame_id = self.frame_id
                    self._valid = True
        finally:
            if bus_owned:
                self._end()
            self.last_slice_us = time.ticks_diff(time.ticks_us(), started)
            self.last_show_us = self.last_slice_us
            self.max_slice_us = max(self.max_slice_us, self.last_slice_us)
        return not self.frame_pending
