> Current implementation: [Gemini Robotics ER 2](../docs/GEMINI_ROBOTICS_ER2.md). Gemini spoken output is retired; wake and user speech remain; the sections below document the previous implementation.

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

Microphone upload pauses while Digit speaks, including gaps between reply
chunks, and resumes 250 ms after the speaker finishes. Buffered microphone
audio is discarded when a reply starts; speech during the reply is not sent
later. Wait until Digit finishes before replying. Local wake-word and Stop
recognition continue running throughout playback.

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
6. **Begin conversation** and the local **Hey Digit** wake word use the saved preview
   setting. Enable action execution only for supervised testing on a safe surface.

**Chat with Digit**, below the camera, sends typed messages directly over the
same Gemini Live connection. Enter sends; Shift+Enter adds a line. A message
starts a conversation with microphone upload off when Digit is idle. Typing into
an already active voice conversation retains its existing microphone and preview
settings. Replies appear in the chat and play through Digit's speaker. Messages
are limited to 1024 UTF-8 bytes; busy/failed requests retain your draft. Queued
messages are cancelled by Stop, connection loss or newer robot control.

Before the first typed message or microphone upload, Gemini's connection setup
loads body welcome context: Digit has finally received the body he
wanted, with a camera, tracks, sensors and an expressive face. It encourages
curiosity, following through with tools when asked, accurate descriptions and
English speech. This is background context, not a separate reply or permission
to move. It leaves saved system instructions untouched and stays available
across conversations on the warm connection, without adding text messages to
follow-up turns, tool results or microphone input. Reconnection loads the same
context again. In particular, the welcome never opens an unfinished text turn
that could leave Gemini waiting instead of processing realtime speech.

When a Live error cancels a task, the task result retains the specific error
after reconnection. `/api/actions` also keeps `live_last_error`, its age in
`live_last_error_age_ms`, and `live_error_count` until the next error or reboot.
These are historical diagnostics; `voice_error` remains the current error.

The background worker waits for Wi-Fi without counting normal startup as a
connection error, and reports clock synchronization while it is pending.
TLS uses software AES: cold-start serial diagnostics showed hardware AES failing
internal DMA allocations (`esp-aes: Failed to allocate memory`) while processing
PSRAM-backed TLS buffers. Software AES removes that transient DMA dependency;
certificate verification and transport encryption remain enabled.
Connection setup sends JSON as bounded 2 KiB WebSocket fragments instead of a
single large write. Setup has a ten-second write budget; a partial write or
cancellation closes that incomplete socket rather than replaying it. Successful
setup resets reconnect backoff to one second (repeated failures cap at 30 seconds).
The dashboard shows secure connection, instruction upload, setup confirmation,
and retry stages, with attempt count/age and the last issue. `/api/actions`
exposes `live_connection_status`, `live_attempts`, and `live_attempt_age_ms`.
HTTP upgrade failures and server close/error codes distinguish key permissions,
quota, rejected settings, and temporary service failures without exposing server
payloads or credentials. **Test connection** interrupts an unsuccessful attempt
or retry wait immediately; a ready connection is verified without opening media.

The camera's bottom-left **Last sent to Gemini** inset shows the exact JPEG
retained after a successful camera upload. It refreshes only after another image
is sent, remains visible after End/Stop, and clears on device restart. Opening
the dashboard or fetching the preview does not capture or upload another image.

**System instructions** has three editable sections: **Personality & behavior**
(reactions and conversational conduct), **Base information** (identity, body,
functions, sensing and operating rules), and **Voice instructions** (tone, pace,
pronunciation and speaking style). Each restore-default button changes
only its own editor; **Save settings** applies all three. Empty sections remain empty;
use the restore buttons to recover defaults. There is no fixed editor or 4 KiB
instruction cap. The dashboard shows the total UTF-8 byte count; instruction
buffers are allocated to fit the document. Available device memory/storage and
the Gemini model's context capacity remain finite.

Saving shows progress immediately and reads the settings back from Digit before
displaying **Saved on Digit — verified**. Requests time out after ten seconds so
the form cannot remain stuck waiting. Unsaved instruction drafts (never API keys)
stay in this browser tab across refreshes, including failed saves. A restored
draft is explicitly marked unsaved until **Save settings** succeeds; closing the
tab clears its draft. Verified instructions remain stored on Digit.

Windows, Unix and mixed line endings load into the same three instruction
editors. Loading normalizes only the displayed text; it does not rewrite the
saved document. Tab drafts affected by the earlier line-ending bug, with all
three headings inside Personality, are separated into their proper editors
while retaining unsaved edits. Save is still required to apply any changes.

The default voice uses clear, slightly higher-pitched speech with restrained
pitch variation, short evenly timed phrases and a gently mechanical rhythm.
Gemini speaker playback also mixes 85% original audio with 15% of a 70 Hz
ring-modulated copy at 16 kHz for a subtle electronic texture. This local effect
adds no audio buffer or delay, cannot amplify sample peaks, and leaves digital
silence silent. Its phase resets when playback is cancelled or a new session
starts. Microphone input and local wake/Stop recognition are not processed by
the effect. Changing Voice instructions changes delivery; the electronic mix
is currently a firmware constant.

All three sections are serialized with headings into `system_instructions`.
The document occupies one of two slots in `gemini_data`, the formerly unused
speech partition at `0x460000`. Each slot has 1,900,544 bytes of physical capacity.
Save writes and reads back the inactive slot before committing its length, CRC
and selector together with the small settings record in NVS. Failed writes keep
the previous slot selected; unchanged instructions are not rewritten. Existing
v1-v4 settings, including API keys and v4 instructions, migrate on the next save.
No partition offsets move, and normal firmware uploads preserve both storage areas.
The old
stock prompt is split automatically in the editor; an older custom combined
prompt is preserved verbatim in the personality editor with a notice to move its
robot facts into the base editor and speaking style into the voice editor. Saved
two-section prompts retain both sections verbatim and start with an empty voice
editor. Nothing is saved by opening the page. API keys,
preview mode and timeout are retained. Local movement limits, task authority and
Stop protections remain enforced in firmware regardless of prompt edits.

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
{"configured":true,"model":"gemini-3.8-live","dry_run":true,"idle_timeout_sec":30,"generation":1,"system_instructions":"...","default_system_instructions":"...","system_instructions_storage_bytes":1900544}
```

It never returns the key. Read the current generation immediately before
controls. All POST requests require an `application/json` object, a matching
integer `generation`. Live controls retain a maximum body length of 1024 bytes;
the settings request budget is derived from physical instruction storage with
room for JSON escaping and credentials, and buffers are allocated to the actual
request size. Allocation or flash failures retain the previous settings. Unknown or
duplicate properties, invalid field types, trailing JSON and cross-origin
browser requests are rejected. A stale generation returns conflict without
performing the requested operation.

| Route | JSON properties besides generation | Behavior |
| --- | --- | --- |
| `POST /api/gemini/settings` | Optional `api_key`, `model`, `dry_run`, `idle_timeout_sec`, `remove_key`, `system_instructions` | Save settings on Digit; blank key/model retain them. Omitted instructions retain them; blank instructions restore defaults. `remove_key:true` cannot accompany a nonempty key. |
| `POST /api/gemini/live/test` | None | Connection-only test; no microphone, camera, output or actions. |
| `POST /api/gemini/live/start` | `mode:"test_only"` or `mode:"conversation"` | Start streaming. Test-only always disables actions; conversation uses saved dry run. |
| `POST /api/gemini/live/stop` | None | End the conversation and cancel streaming, playback, motion and pending tools. |
| `POST /api/gemini/chat` | `text` | Queue one message, using the active session or starting a microphone-free chat. Same generation/origin validation; up to 1024 decoded UTF-8 bytes, 6272 wire bytes. Busy or robot activity returns conflict. |
| `GET /api/gemini/last-image?frame=ID` | No body | Return the exact last sent JPEG for `last_image_id`; unavailable/replaced frames return 404. Never captures or sends a new frame to Gemini. |

Settings changes cancel current work before applying new configuration.
Changing the saved key, model or system instructions reconnects with the new configuration while
keeping microphone/video upload off. Removing the key closes the connection.
Start and connection-test requests reject existing robot activity; stop any
movement, wake turn or manual drive before using these controls.
An ongoing conversation ends on inactivity, End/Stop, settings changes,
manual takeover, a hard safety event or loss of the robot link.
End/ordinary Stop and inactivity disable microphone/video upload, speaker playback and
tools while keeping the service connection ready for the next wake. **STOP ALL**
also closes the current service connection and discards its conversation and
resumption handle. A fresh Gemini connection opens automatically, with media and
actions off until a new wake, Start session or typed message. A network
failure immediately stops motion and mutes media. A healthy task with unchanged
authority/settings may recover within 20 seconds and its original deadline,
confirm rest, upload a fresh image and continue planning. Cancelled tasks cannot
resume and interrupted motor commands are never replayed.
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
STOP ALL increases it after the fresh connection completes setup.

Chat diagnostics include `live_text_only`, `live_text_pending`, `live_thinking`
and `live_response_id`; the latter identifies replies while transcript fragments
arrive. Preview metadata is `last_image_id`, `last_image_bytes` and
`last_image_age_ms`. `task_max_moves` reports the actual 50-movement limit used
by the dashboard counter and local enforcement. Firmware also supplies this
limit to Gemini separately from editable instructions.

## Safety and acceptance

Fresh spoken prompts and successfully sent typed messages automatically open a
local task; follow-ups steer an existing task without renewing its limits.
The first transcript after speech activity opens the task, while later fragments
and model replies cannot reopen finished work. Gemini decides normal completion
with `finish_task`, including for conversational answers without movement.
Navigation requires `observe` before the first movement. One typed steering
message can wait for the outstanding action/result; Stop clears that message.

Tools are `start_task`, `observe`, `turn_left`, `turn_right`, `move_forward`,
`move_backward`, `push_object`, `set_expression`, `finish_task` and `stop`. The old fixed action
registry is retired. `start_task` is optional compatibility for older saved
instructions: it only names/observes an existing task and cannot open or reset one.
Readiness is rechecked at dispatch; Gemini cannot change
safety distances, raw motor commands or UART messages. Matching
local job results and a fresh post-move image drive continuation. Turns are
limited to 90 degrees, forward travel to 30 cm and reverse to 10 cm per move;
tasks have a 90-second deadline and 50-move budget. Distance tools require actual
measured calibration. See [task execution and calibration](../docs/GEMINI_ROBOT_CONTROL.md).

Zumo owns brief sensor tolerance and bounded stopped recovery. A recovering
job remains pending until a terminal result, confirmed rest and a fresh image;
Gemini never issues its own firmware-recovery reverse. Interrupted movements
report actual track travel, uncertainty, recovery cause/count and route
constraints. Recovery counts are diagnostic, without a cumulative cap; at most one edge retreat fits
the original task limits. A blockage requires another heading; turns, reverses
and pushes cannot by themselves clear a navigation detour. Hard safety faults,
Stop and failed recovery still end movement authority.

`push_object(distance_cm)` accepts an integer 1–30 cm forward only, on updated
Zumo firmware with an available edge-retreat allowance and fresh image. It uses the
fixed maximum motor target and high acceleration while bypassing only that
job's close-obstacle threshold. All other safeguards remain active. The tool
reports movement, not proof that an object fell over; Gemini checks the image.
Fresh beyond-range front readings appear as **>1 metre**, clear to continue
subject to other local safeguards. They are distinct from stale/missing data
or actual sensor errors and do not fabricate an exact measured distance.

Fixed navigation instructions ask Gemini to use fresh images and sensors to
figure out how to avoid the obstacle, choosing its own route, turn angles and
movement distances within existing tool limits. Seeing an obstacle or backing away
does not complete exploration. Gemini continues the original task without a
new request. XIAO rejects a completed outcome while a locally detected detour
still lacks renewed forward progress; it always permits a failed/cancelled
outcome when no safe route exists. Passing the object is a visual judgment,
not something the forward progress check can prove.

Floor calibration belongs to the local startup process. Gemini has no calibration
tool or calibration guidance. The dashboard retains a manual retry for failed
startup calibration. After a successful startup, an ordinary pickup/edge stop
can clear locally after one second of normal readings with the original baseline;
cancelled tasks do not resume.

Verify the boot connection with no microphone/video upload before waking Digit.
Then verify
Test-only transmission and audible replies, wake behavior, spoken Stop during
assistant speech, cancellation while a tool is pending, manual takeover and
network failure. Verify End/ordinary Stop and inactivity return to the ready connection,
while STOP ALL creates a fresh idle connection and increases `live_connections`.
Neither path sends media until a new wake, dashboard start or typed message. Run physical action
tests separately under supervision.
Desktop regressions and a successful firmware build do not prove physical
braking, obstacle clearance, wake accuracy or local Stop latency.

### Connection reliability verification, 2026-10-12

The software-AES build and dashboard were installed on XIAO COM7 with verified
flash hashes. Five startup connections each completed on attempt 1 with zero
Live errors; measured setup latency was approximately 1.7–1.8 seconds after
network readiness. The final connection stayed ready through a 60-second idle
heartbeat check without reconnecting. Connection probes sent no microphone or
camera data, played no audio, and ran no robot actions; motors stayed at zero
and local Stop remained ready. Saved system instructions survived the restarts
unchanged. Host regressions also passed for large setup fragmentation, bounded
writes, cancellation, backoff reset, diagnostics and dashboard status. These
checks do not establish acoustic wake/Stop accuracy or physical motion acceptance.

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

### Task continuity during reconnection

A temporary microphone-send error pauses local movement and preserves the
current goal. Connection/error transitions are serialized with the control loop
so clearing input readiness cannot trigger the initial activation timeout before
recovery is prepared. After reconnection, the controller confirms stopped tracks,
uploads a fresh view, and asks Gemini to continue planning under the original goal
and remaining limits. Stop, manual takeover, safety faults, settings changes, and
the existing task/recovery deadlines still cancel; stale movement is not replayed.

Context-window compression and session resumption are already enabled. The Live
connection stays ready between conversations with microphone/camera upload off.
STOP ALL explicitly discards that connection and opens a fresh conversation.
Google may renew the connection while longer sessions continue through resumption;
the local conversation inactivity timer and 90-second robot-task deadline are
separate from the provider's connection lifetime.
