> Current implementation: [Gemini Robotics ER 2](../docs/GEMINI_ROBOTICS_ER2.md). Gemini spoken output is retired; wake and user speech remain; the sections below document the previous implementation.

# Digit voice and robot control

The local **Hey Digit** wake word and independent **Stop** detector stay on the XIAO.
Gemini Live replaces MultiNet. One connection is prepared at boot when Wi-Fi and
a saved key are available; standby sends no microphone audio or camera frames.

Say **Digit**, wait for the visual wake acknowledgement, then speak. An enabled
turn toward the saved voice bearing runs before listening under the same local
readiness/surface gates. An unclear bearing falls back to a stationary reply.
A missing key or failed connection cannot present a false ready acknowledgement.

Each fresh spoken or typed prompt automatically opens a bounded task when none
is active. Follow-up prompts steer the active task and keep its existing limits.
A greeting can receive a spoken reply and expression, then `finish_task`, without
moving or requesting a camera frame. For **Find the battery**, Gemini first calls
`observe`, then chooses individual turns and distance moves and continues through
native tool results. No `start_task` or fresh prompt is required between steps.
The task remains active across model turn boundaries until explicit completion,
failure, cancellation or its limit. See the complete
[robot-control contract](../docs/GEMINI_ROBOT_CONTROL.md).

Camera upload occurs on an explicit observation (required before first movement)
and after a confirmed movement. Each image must have been captured after the
request/rest; stale or unavailable imagery fails an active task. ToF telemetry
includes validity, age and a **>1 metre** beyond-range label, and measures only
the narrow front area.
The optional local person/object overlays remain manual vision tools.

The dashboard retains hold-to-drive arrows, vision buttons, silent facial
expressions, speaker volume and task status. Fixed routines, Turn with person,
audio tests and emotion sounds are retired. Gemini cannot change safety settings
or motor power. Starting limits are 1-90 degrees, 1-30 cm forward and 1-10 cm
backward. The dedicated `push_object(distance_cm)` accepts 1–30 cm forward
only at the fixed maximum target/high acceleration, with its close-obstacle
exception scoped to that job. Zumo handles bounded recovery; Gemini receives
the final outcome and a fresh image. Actual distance calibration is required for travel and is saved for
the final powered setup step.

One ReSpeaker capture task feeds the local wake/Stop detectors and the active
Live session. The right ASR channel supplies 16 kHz mono audio; the ReSpeaker
retains its hardware echo reference. Returned 24 kHz Live audio is resampled to
16 kHz for the existing speaker path. Local Stop receives audio during speech.
Silent face expressions can run alongside speech or a movement.

Dashboard **STOP ALL**, **End conversation**, spoken **Stop** and Zumo **Button C**
cancel their active work. Manual takeover, settings changes, safety events, robot
link loss invalidate the task. Generation/session identities prevent delayed
traffic from restarting cancelled work. Stop and inactivity keep the connection
ready with uploads and cloud output disabled. A transient cloud/Wi-Fi outage
stops motion and can preserve a healthy task under its original limits; after
reconnecting it requires confirmed rest and a fresh observation before planning.
Interrupted motor commands are never replayed. Invalid authority or expired
recovery leaves Digit stopped.

The API key is saved on the XIAO in NVS and never returned to the dashboard.
See [Gemini setup and sanitized API](GEMINI_LIVE.md). Connection tests have no
microphone/camera/output/actions; Test-only conversations disable physical tool
execution. Local safety, feedback and timeouts remain authoritative on Zumo.

Open **xiao-sense** and use **xiao_esp32s3_sense_idf** for builds/uploads. Zumo
Python installation is separate. `GET /api/actions` exposes sanitized conversation,
transcript, task/result and local Stop state. `GET /api/zumo/status` reports sensor
and calibration readiness. Desktop/build/install/idle checks do not establish
physical accuracy, wake detection quality or Stop latency; those need supervised
robot acceptance.
