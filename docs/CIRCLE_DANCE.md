# Circle dance

Start **Circle dance** in the dashboard, or say **Digit**, then **Circle dance**.
The action runs three counterclockwise circles with the existing short Dance
melody repeating while the local motion job is running. It stops the tracks
and music after completion. Dashboard Stop/STOP ALL, manual takeover, and the
audio Stop playback control cancel both.
While moving, the local controller cycles happy, excited, and heart-eye faces
every 1.2 seconds with a six-color light chase at 180 ms steps. Stop, completion,
or a fault restores the prior LED colors/brightness and returns the face to
neutral. Decoration failures are contained and cannot keep motion alive.

The dashboard Drive speed (3000–6000) is captured at action start. Changing
the slider during a dance affects the next dance. Slider changes are sent to
the XIAO so spoken starts use the selected speed too; the startup default is
4000. Acceleration uses the existing dashboard setting.

Both tracks move forward. The outside track uses the selected dashboard speed;
the inside track targets one-third of its measured encoder velocity. This
targets approximately a six-inch-diameter center path, a three-inch-diameter
inside-track-center path, and a nine-inch-diameter outside-track-center path,
assuming about three inches between track centers. Actual diameter depends on
track spacing, startup/braking transients, and surface slip. It needs a
supervised floor measurement before any accuracy claim. The inner edge of the
track/body reaches inside the track-center path, so that three-inch path is
not a guaranteed three-inch object clearance. Position Digit alongside the
object before starting; this preset does not find or track the object.

Encoder control applies the 1:3 ratio after converting dashboard speed to
counts/second. The inside track receives startup torque, then may use lower
power under feedback to maintain its slower rate. Stall and encoder-direction
checks remain active. Stop and manual takeover clear the ratio immediately.

The Zumo gyro counts 1080 degrees with braking lead and an eight-degree final
tolerance. A local 60-second deadline and bounded settling retries prevent
indefinite circling. Startup gyro calibration holds the motors at zero. The
job requires calibrated surface sensors, encoder feedback, gyro feedback,
and fresh front sensor observations: a valid distance of at least 50 mm or
ST status 2 (weak/no return) allows circling. A no-return observation is not
a measured distance or a guarantee of empty space; dark or small targets can
also produce weak returns. The dashboard labels it **ToF: no return**.
The obstacle stop triggers strictly below 50 mm (5 cm), matching the other
automatic movement paths. Manual dashboard drive/turn has no front-distance
stop. Normal action arrival targets are separate from this obstacle threshold.
Every completed sensor result has its own timestamp, so invalid distance
and stale sensor updates are distinct. Brief unreliable samples get at most
250 ms since the last usable reading to recover. Below-detection, hardware,
and unknown-status errors stop immediately; persistent unreliable readings,
no updates for 250 ms, or I2C failure also stop. Specific RANGE_STATUS_n,
RANGE_STALE, and RANGE_IO results distinguish these cases. Fault reports retain
the sensor status and raw distance that triggered them, even after recovery.
Actions that
require a measured approach distance continue to require valid measurements.
Edge, Button C, stale/near range,
encoder faults, gyro faults, incorrect turn direction, lack of yaw progress,
control-loop delay, and lost supervisor heartbeat stop the job.

Protocol: action ID 16; capability `CIRCLE=2`; persistent job
`CIRCLE speed 3 acceleration max_ms` in the existing M2 session protocol.
The peer reports `CIRCLE_DONE` after stopping. Older peers do not enable the
dashboard action. Update `circle_show.py`, `motion_jobs.py`, `encoder_speed.py`, and `uart_control.py` on the Zumo
alongside the XIAO firmware; preserve backups and verify installed files.

Desktop tests cover selected speed, three-lap completion, duplicate-job
suppression, music lifetime, cancellation, and failure paths. Build/install
checks do not establish physical circle diameter or stopping accuracy.
