# Digit: Gemini Robotics ER 2

Digit now uses `gemini-robotics-er-2-streaming-preview` through a certificate-verified, persistent WebSocket on the XIAO. This replaces Gemini spoken replies while preserving wake-word activation and spoken input. No PC is required to run the robot.

## Interaction

- **Start session** starts continuous camera input and spontaneous emotion reactions. Say Hey Digit for the local acknowledgment and spoken input. Starting alone does not authorize navigation.
- **Chat with Digit** sends typed messages. A fresh spoken request or typed message opens a bounded task, or steers the current task without resetting its limits.
- Digit also sends brief text to **Chat with Digit**: observations, intended next steps, meaningful results and reasons for changing course. When observing and free to reply, the controller requests an update after six seconds without text. These updates never start tasks or interrupt a pending tool; delivery depends on Gemini's response time.
- **Test connection** validates setup without camera uploads, sounds, gestures or actions.
- **Test-only stream**, or the saved **Preview robot actions only** setting, permits camera input and model decisions but starts no expression, sound or movement.
- **End session / STOP ALL** cancels the session, camera uploads, emotion sounds, gestures and pending work. Cancelled work cannot restart from delayed packets or tool results.
- **STOP ALL** also closes the current Gemini connection and opens a fresh one with no conversation resumption. The new connection stays idle with microphone/camera upload and actions off until a new wake, Start session or typed message. End session keeps the existing idle connection.
- `finish_task` ends navigation authority while leaving the observation session open. The session has no silence timeout. The timeout setting limits an unresponsive model turn; movement task deadlines remain independent.

Gemini speech output and TTS are disabled. Hey Digit and emergency Stop remain local. Spoken commands stream directly to ER 2 as 16 kHz mono PCM; there is no local command vocabulary parser. The I2S driver uses RXTX for microphones and local emotion/wake sounds. The processed microphone channel feeds Gemini; every captured frame still reaches local Stop. Wake acknowledgment and emotion cues, including their acoustic tails, are excluded from cloud microphone input. Typed chat opened from idle remains microphone-free; Start session or waking Digit enables spoken input.

## Action Console

The dashboard's Action Console, below Chat with Digit, records function calls
including spontaneous `set_expression` calls. Each row shows its source,
arguments, short public explanation and actual status/result. Gemini is asked
to attach `action_reason` (up to 191 UTF-8 bytes) to ordinary calls. This optional
metadata never authorizes an action; older calls and emergency `stop()` remain
valid without it and display "No reason provided". `finish_task.reason` remains
the task outcome explanation and is separate from `action_reason`.

Face results become Applied only after Zumo's display acknowledgement. That
acknowledgement does not confirm a gesture or sound. Requests can instead become
Blocked, Rejected, Failed, Cancelled or Test only. Controller limitations are
separate from Gemini's explanation. Stop and connection loss cancel unfinished
calls; delayed results cannot turn them into successful actions. Dashboard face
previews, manual directions and important local stop/recovery events also appear.

The history retains 50 rows in 56,400 bytes of PSRAM, survives closing the page,
and resets when the XIAO restarts. Allocation failure disables diagnostics only.
The read-only `/api/actions/console?after=<revision>&boot=<boot_id>` endpoint
returns changed retained rows and retention/reboot metadata. The page polls once
per second while visible; it never sends commands to populate the console.
Emotes/Movement/Other filters, scroll pause, copy and JSON download operate on the
retained history. The separate motion debug log remains available.

## Camera and model turns

The camera broker supplies the newest JPEG, bounded to 64 KiB. Uploads share one minimum 1000 ms interval, including ordinary frames, `observe` requests and frames after a stopped movement. The timer also spans socket reconnects. Frames continue during reasoning and movement. Congestion drops opportunities rather than queuing old frames or sending catch-up bursts. A requested observation waits for a capture newer than its request/rest barrier.

Frame uploads alone do not trigger reasoning. When the model is idle, has no pending blocking tool, and a fresh image is available, the controller sends a compact internal observation prompt. It includes frame identity, task authority and sensor status. It never emits a user-prompt event, creates a task, renews task limits or interrupts an in-flight turn. User speech and typed input have priority; the heartbeat waits during speech/VAD, reasoning and pending tools. No useful change can produce no reply/tool.

ER 2 responses are read from native text parts; thinking parts and inline media are not sent to the speaker. All declared tools use `BLOCKING`. Tools remain strictly allowlisted, argument-validated, deduplicated, session-tagged and executed by the local control loop. The firmware sends actual function responses. Local Stop always preempts tools independently of cloud scheduling.

## Emotions and safety

`set_expression(expression, duration_ms)` retains all thirteen existing faces, quiet synthesized cues and eligible local body gestures. Different model-selected emotions have a 3-second cooldown; repeating the same emotion has an 8-second cooldown. Usually the model holds a face for 2500–6000 ms. Manual previews remain available.

Gestures and sounds are gated by the local controller. During driving, only the face animates. EDGE still permits display-only expressions while blocking ordinary motion; other faults and Stop retain priority. The expression result confirms face visibility and explains that body/sound depend on local eligibility; it does not fabricate physical sound or movement confirmation.

Zumo retains immediate sensor checks, measured motion, watchdogs, bounded recovery, edge handling and the narrowly scoped PUSH contract. Cloud reasoning at 1 FPS does not replace those controls. A movement requires an active task, an initial matched observation, a recent uploaded frame and local readiness. Reconnection never replays a motor command. Eligible active tasks recover only with their original limits, confirmed rest and a fresh observation; Stop, configuration changes, expired authority and hard faults prevent recovery.

## Settings and dashboard

The dashboard shows **Personality & behavior**, **Base information**, and **Expression & emotions**. Existing v5 settings using the voice model retain their API key and instruction-slot data while loading the new model. The old voice-instruction section is replaced with emotion instructions in memory, supporting both LF and CRLF documents; custom personality, facts and additional sections are retained. Runtime capability instructions override custom demands for unavailable speech. The next Save writes the migrated document through the existing alternating, CRC-checked, read-back-verified storage slots. Loading does not write flash.

`idle_timeout_sec` is retained in the settings API/on-flash layout for compatibility; its UI meaning is now **Model response timeout**. Save still shows progress and verifies the returned document. The uploaded-image preview retains the exact last successfully sent bytes, including after End/Stop.

Primary code: `xiao-sense/src/robot_session.cpp`, `gemini_live.cpp`, `gemini_client.cpp`, `gemini_prompt.h`, `audio_engine.cpp`, `web_server.cpp`. Local wake/Stop detector sources and trained models remain built. Gemini speech decoding, resampling and playback are excluded; local emotion sounds remain independent.

## Local validation and installation

Run the relevant suites under `xiao-sense/tests`: `run_gemini_live.ps1`, `run_robot_session.ps1`, `run_gemini.ps1`, `run_gemini_config.ps1`, `run_actions.ps1`, `run_local_stop.ps1` and `run_gesture_chirp.ps1`. Browser checks use `test_dashboard.cjs`, `test_gemini_instructions.cjs` and `test_gemini_chat.cjs`; set `NODE_PATH` to the bundled Node package directory if needed.

Build/check the `xiao_esp32s3_sense_idf` environment with PlatformIO. `check_bundle` prints the complete upload command without executing it. Generate a device-free dashboard preview with `tools/preview_dashboard.cjs`.

Builds and mock/browser tests do not establish real ER 2 API access, on-device sustained upload rate, audible cues or physical gesture/safety behavior. After preview approval and installation, start with Test connection, then Test-only stream, then stationary expression checks and supervised physical acceptance. The coordinated original-emotion restoration also updates Zumo gesture-end reporting so completed gestures do not truncate their richer sounds. Install both boards together for that sound change.

Provider references: [Robotics streaming](https://ai.google.dev/gemini-api/docs/robotics-streaming), [model capabilities](https://ai.google.dev/gemini-api/docs/robotics-overview).

## Activity display and dance

The session card shows connecting/reconnecting, wake acknowledgment, message queued, thinking/waiting for Gemini, receiving a reply, waiting for an action result, observing and stopped/error states. Labels and explanations accompany the colors; pending replies show elapsed waiting time. These indicate transport/controller activity, not private model reasoning. The last connection issue remains visible separately from current activity.

The blocking `dance` tool accepts no arguments and uses command ID 29. It requires an active user task, a matched fresh observation and local movement readiness. Zumo negotiates `DANCE=1` and receives one checksummed `M2 … DANCE 8000` job. Only its matching first actual pulse (`DANCING`) starts the XIAO's original 4.8-second electronic melody. Duplicate or stale replies cannot restart the music or tracks.

The local choreography waits for a complete Happy frame, then performs six balanced side-to-side wiggles: opposing track targets of ±3000, 80/160 ms pulses and 160 ms stopped pauses. RGB colors flash at 100 ms intervals and the previous LED settings are restored at completion/cancellation. It requests no forward translation; timed turns do not establish exact restored heading, so position certainty is reported false. Completion waits for encoder-confirmed rest and a new camera image. A dance consumes an action budget but never clears navigation no-progress or detour limits.

Floor/range/encoder checks, Button C, Stop, scene replacement, controller-loop gap, job deadline and the independent supervisor lease cancel the routine. It never performs a recovery movement on a dance fault. The music is allocation-free, follows the approved emotion/wake player cancellation path, and generates no speech. Test-only/dry-run cannot start its tracks, lights or sound.

## Streaming interruption diagnosis

Real-device test-only streaming reproduced camera/audio write failures with only 399 bytes of internal RAM free (largest block 152), while more than 7 MB PSRAM remained available. Eligible Wi-Fi/lwIP allocations now prefer PSRAM, ordinary allocations over 1 KiB prefer PSRAM, and 64 KiB is reserved for internal/DMA needs. PSRAM networking selects fixed Wi-Fi TX buffers; the default pool could not initialize on this board, so the paired configuration explicitly uses six static RX buffers, four static TX buffers and a bounded PSRAM TX queue.

WebSocket headers and payloads are separate TLS writes in ESP-IDF. Enabling `TCP_NODELAY` on the Live socket prevents small-packet delays from making image uploads overrun the 500 ms microphone queue. Media failures now include the failed fragment size/offset, and `/api/status` reports free/largest internal memory. Upload scheduling uses one-second starts, includes reconnects and never queues old frames to catch up. The initial low-latency firmware passed 65 seconds of simultaneous microphone/video with zero connection errors, zero dropped audio and stopped motors. Full evidence is under `xiao-sense/test-artifacts/er2-interruption/`.

Allocation/socket references: [ESP-IDF PSRAM configuration](https://docs.espressif.com/projects/esp-idf/en/v5.2.5/esp32s3/api-reference/kconfig.html#config-spiram-try-allocate-wifi-lwip), [TCP socket options](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/lwip.html#socket-options).

## Installation on October 12, 2026

The paired update was installed on the verified XIAO (COM7) and Zumo (COM6). The XIAO firmware/model upload passed flash hash verification. Zumo runtime files were backed up, staged, compiled on the device, imported and hash-verified; saved encoder/distance calibration was preserved. The installed Windows-line-ending instruction document exposed a migration issue, which was corrected, covered by LF/CRLF preservation tests and included in the final XIAO upload.

Three GET-only idle checks confirmed Zumo ready with fault NONE, zero motor targets and encoder rates, no active task, and ready audio, camera, wake word and local spoken Stop. The device completed ER 2 Live setup with its existing key and reported one successful connection with no connection errors. Its custom personality/base prefix matched the pre-install hash. No microphone or video streaming session, sound audition, gesture or movement test was started. Evidence and the backup manifest are under `xiao-sense/test-artifacts/er2-install/`.

The subsequent interruption fix, activity display and dance update were also installed on both boards. The 2,670,464-byte XIAO image passed flash hash verification. Zumo's new dance controller and updated job/UART files were backed up, compiled, imported and hash-verified, with calibration preserved. Three stationary checks confirmed negotiated dance support, Zumo ready with fault NONE and stopped motors. The 404 Zumo tests, native action/Live/session/audio tests and dashboard browser checks passed.

The final firmware's test-only microphone/video run reached 50 frames in 50.8 seconds (reported 0.97 FPS), with 810,240 microphone samples, zero new connection errors, no reconnects and no dropped microphone frames. Motor targets and encoder rates stayed zero. The user pressed dashboard STOP ALL before the requested 65 seconds and confirmed the stop was intentional; the raw test records this early ending, and no further stream or model-call test was started. Digit remains stopped. Physical dance, audible playback on the robot and an actual Gemini dance tool call remain untested. The paired deployment and final check summary are under `xiao-sense/test-artifacts/er2-dance-install/`.

## Action Console installation on October 13, 2026

The Action Console app was installed on the identified ESP32-S3 at COM7 after
backing up its existing app partition. The 2,682,912-byte image passed flash hash
verification. Only the app partition was written; public settings read-back
confirmed the saved model, preview setting, timeout and system instructions were
unchanged. No Zumo files were installed.

Native Live/transport/session/action/UART/local Stop tests and dashboard browser
checks passed, including explanations, face acknowledgements, cancellation,
deduplication, test-only calls, retention and reboot recovery. Desktop/mobile
previews rendered successfully. Three GET-only device checks confirmed the new
console endpoint and dashboard, allocated history buffer, successful idle Gemini
connection, local wake/Stop readiness, Zumo fault NONE and zero motor targets.
No media session, face/gesture/sound or movement acceptance test was started.
Build, backup, flash and idle evidence are under
`xiao-sense/test-artifacts/action-console-*`.
