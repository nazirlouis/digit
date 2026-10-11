# Gemini Live on Digit

Digit connects directly from the XIAO to Gemini Live over a certificate-verified
WebSocket after boot when a saved key and Wi-Fi are available. The connection
stays ready while microphone and camera upload remain off. The local wake word
activates a conversation after any enabled turn toward the saved voice direction.
Digit shows a visual wake acknowledgement once Gemini is ready. Wait for it
before speaking. Microphone audio provides conversation input. A fresh JPEG is
uploaded on request, when starting a task and after each completed movement.
The assistant replies through Digit's speaker and requests bounded robot tools.
The Zumo retains motor feedback, safety and deadlines.

## Setup

1. Open Digit's dashboard on a trusted local network and find **Gemini Live setup**.
2. Enter a Gemini API key and save. A blank key keeps the stored key. The input
   clears after a successful save; the dashboard never retrieves the saved value.
3. Keep **Preview robot actions only** checked for initial testing. The model
   is `gemini-3.8-live`; the idle timeout defaults to 30 seconds, range 10–300.
4. Use **Test connection** first. This checks the ready service connection or
   reconnects if needed:
   microphone, camera, speaker output and robot actions are disabled.
5. Use **Test-only stream** to exercise the microphone, requested observations and
   speaker replies with actions always disabled. Use **End conversation** to stop.
6. **Begin conversation** and the local **Digit** wake word use the saved preview
   setting. Enable action execution only for supervised testing on a safe surface.

The key is stored in XIAO NVS and persists across restarts and normal app
uploads that retain NVS. **Remove saved key** explicitly deletes it. This is
not a claim of encrypted storage: the dashboard itself uses HTTP, so key entry
is protected by the security of your local network. Never put a real key in
source, screenshots, scripts or repository files. Narrow ignore patterns cover
local Gemini credential files, `secrets/` and named NVS/flash dumps; deployable
model binaries remain tracked.

## Dashboard API

`GET /api/gemini/settings` returns only sanitized settings:

```json
{"configured":true,"model":"gemini-3.8-live","dry_run":true,"idle_timeout_sec":30,"generation":1}
```

It never returns the key. Read the current generation immediately before
controls. All POST requests require an `application/json` object, a matching
integer `generation`, and a maximum body length of 1024 bytes. Unknown or
duplicate properties, invalid field types, trailing JSON and cross-origin
browser requests are rejected. A stale generation returns conflict without
performing the requested operation.

| Route | JSON properties besides generation | Behavior |
| --- | --- | --- |
| `POST /api/gemini/settings` | Optional `api_key`, `model`, `dry_run`, `idle_timeout_sec`, `remove_key` | Save settings on Digit; blank key/model retain them. `remove_key:true` cannot accompany a nonempty key. |
| `POST /api/gemini/live/test` | None | Connection-only test; no microphone, camera, output or actions. |
| `POST /api/gemini/live/start` | `mode:"test_only"` or `mode:"conversation"` | Start streaming. Test-only always disables actions; conversation uses saved dry run. |
| `POST /api/gemini/live/stop` | None | End the conversation and cancel streaming, playback, motion and pending tools. |

Settings changes cancel current work before applying new configuration.
Changing the saved key or model reconnects with the new configuration while
keeping microphone/video upload off. Removing the key closes the connection.
Start and connection-test requests reject existing robot activity; stop any
movement, wake turn or manual drive before using these controls.
An ongoing conversation ends on inactivity, End/Stop, settings changes,
manual takeover, a safety event, loss of the robot link or transport failure.
End/Stop and inactivity disable microphone/video upload, speaker playback and
tools while keeping the service connection ready for the next wake. A network
failure can reconnect automatically, but it never automatically restarts media
upload, assistant playback or a cancelled action.
If a pending response cannot be confirmed interrupted, recovery also renews the
connection. This is an exceptional recovery path, not the normal wake/Stop flow.
Queued audio, video and tool results carry session/generation identity;
reconnection cannot replay an old action. Local recording/replay/tone controls
have been removed; the volume slider remains.

`GET /api/actions` reports sanitized session state through `live_active`,
`live_connected`, `live_connecting`, `live_connections`, `live_speaking`, `live_probe`, `live_dry_run`,
`live_latency_ms`, sent/received/played audio sample counters, camera frame/byte
counters, capture age, remaining idle time, transcript, response and reason.
Those fields describe software progress; counters alone do not establish
audible playback or visible camera quality.
`live_connected:true` with `live_active:false` means the connection is ready
and microphone/camera upload is off; it does not mean a conversation is running.
`live_connections` counts confirmed service connections since boot. It stays
unchanged when an ordinary Stop/inactivity and the next wake reuse the connection;
an actual reconnect increases it.

## Safety and acceptance

Tools are `start_task`, `observe`, `turn_left`, `turn_right`, `move_forward`,
`move_backward`, `set_expression`, `finish_task` and `stop`. The old fixed action
registry is retired. Readiness is rechecked at dispatch; Gemini cannot change
calibration, safety distances, raw motor commands or UART messages. Matching
local job results and a fresh post-move image drive continuation. Turns are
limited to 90 degrees, forward travel to 30 cm and reverse to 10 cm per move;
tasks have a 90-second deadline and 12-move budget. Distance tools require actual
measured calibration. See [task execution and calibration](../docs/GEMINI_ROBOT_CONTROL.md).

Verify the boot connection with no microphone/video upload before waking Digit.
Then verify
Test-only transmission and audible replies, wake behavior, spoken Stop during
assistant speech, cancellation while a tool is pending, manual takeover and
network failure. Verify End/Stop and inactivity return to the ready connection
without sending media until a new wake or dashboard start. Run physical action
tests separately under supervision.
Desktop regressions and a successful firmware build do not prove physical
braking, obstacle clearance, wake accuracy or local Stop latency.

### Earlier persistent-connection verification, 2026-10-10

The following records the earlier continuous-camera implementation, before the
robot-tool update switched video to requested observations. It is not acceptance
evidence for the new motion tools.

The persistent-connection firmware and vision bundle were installed on XIAO
COM7 with verified flash hashes. The saved key survived the upload; the device
connected directly to Gemini with certificate validation. A connection-only
probe transmitted no microphone/video and kept connection count 1.

Stationary test-only streaming uploaded 480,000 microphone samples and 30 JPEG
frames before the 30-second inactivity timeout. A second activation uploaded
171,520 samples and 11 frames before explicit Stop. Both retained connection
count 1; counters stayed unchanged afterward, with zero dropped audio frames,
no active motion and both motors at zero. The device remained healthy past
135 seconds uptime. These quiet tests produced no assistant audio, so audible
replies and spoken wake/Stop acceptance remain unverified.

Host tests cover settings persistence/migration, API validation, local wake/Stop
gates, persistent transport, stale output/tool cancellation, resumption,
audio/video ownership and existing robot action safety. Control tasks use a
small activity snapshot rather than copying transcripts onto their stacks.

Gemini server packets do not carry a client activation identifier. Local epochs,
fresh speech/transcript evidence and cancellation barriers limit stale output;
local audio energy also marks speech that may precede server VAD. That energy
check is conservative, not proof of perfect attribution for very faint speech.
Unresolved pending response boundaries renew the connection with media off.
Early assistant output is bounded to about two seconds until the input
transcript arrives; exceeding the buffer ends the stream safely.
