# Gemini Robotics ER 2 setup

Digit uses `gemini-robotics-er-2-streaming-preview` directly from the XIAO over a
certificate-verified WebSocket. No PC relay is required. Spoken/typed input and
camera images go to Gemini; responses are dashboard text, expressions, local
electronic sounds, and eligible gestures. Spoken output and TTS are disabled.
See the [full project setup](../README.md) and [ER 2 contract](../docs/GEMINI_ROBOTICS_ER2.md).

## First connection

1. Build/install the active `xiao_esp32s3_sense_idf` project and open the dashboard
   address printed in its serial log.
2. Obtain a [Google AI Studio key](https://aistudio.google.com/apikey) with access
   to this streaming model. Standard ER 2 and old voice-model endpoints differ.
3. In **Session setup**, enter the key and leave **Preview robot actions only** on.
4. Press **Save settings** and wait for the verified result. A blank key retains
   the existing one; successful Save clears the input. The stored key is never
   returned to the browser.
5. Run **Test connection**. Camera/microphone uploads, faces, sounds, gestures,
   and motors remain off.
6. Run **Test-only stream** for microphone/image input and dashboard replies.
   Physical tool dispatch remains disabled even if Gemini requests an action.
7. End the session before performing supervised hardware acceptance and calibration.

Use a trusted local network: the dashboard uses HTTP. Cloud transport is TLS;
status/error messages redact credentials. Check model access, quota, network,
clock synchronization, and the displayed connection issue when setup fails.
Google's [streaming documentation](https://ai.google.dev/gemini-api/docs/robotics-streaming)
describes the required endpoint and its 16 kHz PCM, JPEG, and text modalities.

## Sessions and tasks

**Start session** enables camera/spoken input using the saved preview setting.
**Hey Digit** is recognized locally and opens spoken input after acknowledgment.
**Chat with Digit** starts a text session with microphone upload off when idle;
messages sent into an existing voice session retain that session's mode. Enter
sends; Shift+Enter adds a line. Messages are limited to 1024 UTF-8 bytes. One
steering message can wait for the current tool; Stop cancels it.

The camera sends the newest JPEG at **1 FPS** during active sessions, including
while Gemini reasons or a local action runs. Ordinary frames, requested
observations, post-motion frames, and reconnects share the same 1000 ms interval.
Congestion reduces the actual rate without queues or catch-up bursts. The
**Last sent to Gemini** inset retains the exact successful upload bytes, survives
End/Stop, and clears on reboot. Fetching the preview never triggers an upload.

A fresh user request opens a bounded task. Follow-ups keep its original limits.
Starting a session/background observations never authorize navigation. Movement
needs an initial matched observation and local readiness. Actual results and
post-motion images return before the next step. `finish_task` ends navigation
while observation can continue. The session remains open until End/Stop;
**Model response timeout** limits an unresponsive model turn, not user silence.

**End session** cancels uploads, outputs, tools, and task authority while retaining
the ready connection. **STOP ALL** also cancels manual/local work and opens a
fresh cloud connection without session resumption. Neither permits delayed
packets to restart motion. Standby sends no media. Connection loss stops motion;
eligible recovery retains the original task limits, confirms rest, and requires
a fresh observation. Interrupted motor commands are not replayed.

## Instructions and storage

Edit **Personality & behavior**, **Base information**, and **Expression & emotions**.
Each restore button changes only its draft section; Save applies the document.
Local tool limits and safety are enforced independently of editable instructions.

The document has no fixed 4 KiB editor limit. Flash capacity, device memory, and
model context remain finite. Save writes and reads back the inactive instruction
slot before committing its CRC/selector and small NVS settings record. Failed
writes preserve the selected document. The UI verifies read-back before showing
**Saved on Digit — verified**. Normal application uploads retain key/settings;
whole-flash erasure removes them.

Unsaved instruction drafts, never API keys, remain in the browser tab across
refreshes and failed saves. Restore is not Save. Existing settings retain the key
and custom personality/base sections while migrating old voice instructions to
the current emotion section; runtime capability policy prevents obsolete prompts
from enabling unavailable speech. Read/write API compatibility retains
`idle_timeout_sec`, now interpreted as model response timeout.

The **Action Console** records requested calls, brief public explanations, and
actual controller outcomes. Face acknowledgement proves display application;
body gestures/sound have their own local eligibility gates. Model prose alone
does not establish a physical action succeeded.
