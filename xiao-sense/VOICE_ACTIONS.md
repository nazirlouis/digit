# Digit voice, emotes, and Stop

The active build uses local **Hey Digit** and an independent **Stop** detector,
with direct audio input to Gemini Robotics ER 2. MultiNet and Gemini spoken
output/TTS are not built. See [project setup](../README.md),
[Gemini setup](GEMINI_LIVE.md), and [current behavior](../docs/GEMINI_ROBOTICS_ER2.md).

## Spoken and typed input

Say **Hey Digit**, wait for the wake acknowledgment, then speak. An enabled turn
toward the speaker is gated by local readiness and floor safety; an uncertain
bearing does not force a turn. A missing key or failed cloud connection cannot
produce a false ready acknowledgment.

The XIAO sends 16 kHz mono PCM from the reSpeaker's processed left channel.
Every captured frame still reaches local Stop while sounds/actions are active.
Local acknowledgment/emotion sounds and their acoustic tails are excluded from
cloud microphone input. Digit responds with short dashboard text, expressions,
electronic sounds, and safe gestures, rather than spoken replies.

Typing into **Chat with Digit** starts a microphone-free session from idle;
typing during an active voice session retains its existing mode. Fresh spoken
or typed input opens a bounded task; later input steers it without resetting
limits. Model turns, tool replies, and camera/heartbeat context do not open tasks.

During active sessions camera uploads run at **1 FPS**, including during movement
and reasoning. An initial matched `observe` result is still required before
movement. Post-motion results wait for confirmed rest and a newly captured image.
The optional person/object overlays are independent manual vision tools.

## Actions and cancellation

Tools include observations, 1–90° turns, 1–30 cm forward travel, 1–10 cm backward
travel, expressions, dance, and a dedicated 1–30 cm forward push. Distance travel
requires measured calibration. Push exempts only its close-obstacle threshold;
local edge, feedback, communication, deadlines, and Stop remain authoritative.
Each tool returns an actual result before Gemini plans another step.

The thirteen expressions are NEUTRAL, HAPPY, SAD, MAD, SERIOUS, CURIOUS,
SURPRISED, SLEEPING, CONFUSED, SLEEPY, EXCITED, CRYING, and LOVE. Non-neutral
emotions have procedural electronic sounds; eligible idle expressions add a
bounded body gesture. During driving only the face animates. EDGE permits
display-only expressions while ordinary motion remains blocked. Other faults
and Stop retain priority.

Spoken **Stop**, dashboard **STOP ALL / End session**, and Zumo **button C** cancel
active work. Manual takeover, settings changes, hard faults, and link loss also
invalidate or suspend work under local rules. Session/generation identities
reject stale callbacks. STOP ALL replaces the cloud connection; it returns idle
with microphone/camera uploads and actions off. No cancelled action resumes.

`finish_task` ends navigation authority but leaves observation open. The session
has no silence timeout; End/Stop closes it. Model response waiting and motion
budgets have separate limits.

Use Test connection first (no media/output/actions), then Test-only stream
(physical dispatch disabled). Acoustic wake/Stop accuracy and latency, gestures,
sounds, and movement need supervised real-device checks. Builds/mock tests do
not establish physical acceptance. The sanitized `/api/actions` and
`/api/zumo/status` routes expose readiness and actual task/controller outcomes.
