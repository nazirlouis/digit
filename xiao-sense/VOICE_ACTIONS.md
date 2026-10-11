# Digit voice and robot control

The local **Digit** wake word and independent **Stop** detector stay on the XIAO.
Gemini Live replaces MultiNet. One connection is prepared at boot when Wi-Fi and
a saved key are available; standby sends no microphone audio or camera frames.

Say **Digit**, wait for the visual wake acknowledgement, then speak. An enabled
turn toward the saved voice bearing runs before listening under the same local
readiness/surface gates. An unclear bearing falls back to a stationary reply.
A missing key or failed connection cannot present a false ready acknowledgement.

A greeting can receive a spoken reply and silent expression without starting a
task. A request such as **Find the battery** starts a bounded task. Gemini chooses
individual turns and distance moves, observes afterward, and continues through
native tool results. No fresh spoken prompt is required between those steps.
The task remains active across model turn boundaries until explicit completion,
failure, cancellation or its limit. See the complete
[robot-control contract](../docs/GEMINI_ROBOT_CONTROL.md).

Camera upload occurs on an explicit observation, the initial task observation
and after a confirmed movement. Each image must have been captured after the
request/rest; stale or unavailable imagery fails an active task. ToF telemetry
includes validity, age and no-return, and measures only the narrow front area.
The optional local person/object overlays remain manual vision tools.

The dashboard retains hold-to-drive arrows, vision buttons, silent facial
expressions, speaker volume and task status. Fixed routines, Turn with person,
audio tests and emotion sounds are retired. Gemini cannot change safety settings
or motor power. Starting limits are 1-90 degrees, 1-30 cm forward and 1-10 cm
backward. Actual distance calibration is required for travel and is saved for
the final powered setup step.

One ReSpeaker capture task feeds the local wake/Stop detectors and the active
Live session. The right ASR channel supplies 16 kHz mono audio; the ReSpeaker
retains its hardware echo reference. Returned 24 kHz Live audio is resampled to
16 kHz for the existing speaker path. Local Stop receives audio during speech.
Silent face expressions can run alongside speech or a movement.

Dashboard **STOP ALL**, **End conversation**, spoken **Stop** and Zumo **Button C**
cancel their active work. Manual takeover, settings changes, safety events, robot
link loss and transport failure invalidate the task. Generation/session identities
prevent delayed traffic from restarting cancelled work. Stop and inactivity keep
the connection ready with uploads and cloud output disabled. Reconnection remains
inactive until wake or dashboard start and never resumes an old task.

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
