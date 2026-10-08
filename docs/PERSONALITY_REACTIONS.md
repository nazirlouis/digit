# Digit's droid reaction library

This source update is prepared for local review. It has not been installed on
either controller. The original approved five previews remain at
`xiao-sense/test-artifacts/voice-concepts/face-sync/`; the complete production
preview is at `xiao-sense/test-artifacts/voice-concepts/personality/`.

## Authoring and timing

`reactions/digit_reactions.json` is the authored source for all 13 expressions,
exact sound contours, note/gap lengths, profile gains, and facial timing.
`tools/generate_reactions.py` generates `xiao-sense/src/reaction_catalog.h` and
`zumo-controller/reaction_catalog.py`. Never edit those generated files by hand.
The generator's `--check` mode runs during XIAO configuration and host tests.
Both outputs contain the same catalog fingerprint; Zumo advertises `REACT` in
status and XIAO skips mismatched reactions without failing the navigation task.

The five approved motifs retain their pitch contours and note/gap lengths.
Additional voices cover neutral, sad, serious, surprised, sleeping, sleepy,
crying, and love. Profile gains only attenuate the oscillator output; speaker
volume settings are unchanged. Expressions include a short quiet settling tail.
Sleeping holds its face but plays its sound once; ordinary idle remains quiet.

The XIAO `reaction_player` owns one reaction at a time. It sends SCENE or WAKE,
waits for the corresponding visible-frame acknowledgment, then starts audio.
Zumo begins the face timeline after transferring its first complete frame.
This is acknowledgment-based synchronization, not a shared hardware sample
clock: actual link, speaker, and panel latency still require physical review.

Stop, replacement, and manual takeover invalidate pending reactions. Decorative
failures are bounded skips, not navigation failures. Wake preparation has one
1500 ms budget, followed by a 780 ms reply. Speech also bounds its optional-voice
wait; on timeout it cancels decoration and opens the microphone after the echo
tail instead of discarding the user's command window. The Listening label is
published only after the 120 ms echo tail.

Gesture audio profiles are generated and auditionable, but the coordinated
player currently rejects Gesture requests: SCENE has no gesture-profile wire
field. Wake timing is accepted only through Source::Wake and WAKE. This prevents
a short sound from being paired accidentally with a full-length face.

## Behavior

- The three active missions and dashboard previews share the coordinator.
- Maze scan curiosity sounds have an 8-second cooldown; explicit previews replay.
- Recognition and arrival have separate reactions. Driving does not wait for a
  decorative performance; motor safety and mission supervision remain independent.
- Arrival celebrations use face and sound. Autonomous decorative body wiggles
  are disabled pending a separate movement preview and physical clearance review.
- A startup greeting requires one second of fresh, calibrated, stopped, quiet
  readiness, and expires rather than appearing unexpectedly much later.
- A missed command can produce one confused reaction, only for the same control
  generation and the exact unrecognized-command result. It never retries speech
  or starts an action automatically.
- Fault reactions require a real fresh fault transition and confirmed zero
  motor targets, output, and encoder speeds. They are bounded and rate-limited.
- User Stop mutes pending ambient events until another explicit interaction.
  Automatic safety stops can permit one fault notification after stopping.
- Idle blinking/glancing is retained. No periodic idle chatter or guessed
  low-battery threshold is added. Battery chemistry and a validated threshold are
  required before a low-battery voice can be enabled.

## Local review and validation

Run `xiao-sense/tests/run_emotions.ps1` to export audio directly from the new C++
synthesizer. Run `tools/render_personality_preview.py` with Pillow and FFmpeg
available to render the actual shared face engine with those exported WAVs.
The gallery contains 13 faces, a wake/listening illustration, and three condensed
action storyboards. Storyboards illustrate reaction order; they do not simulate
sensors, travel duration, or physical motion.

Desktop validation covers generated-catalog agreement, exact audio duration and
headroom, all face timelines, stale/late acknowledgments, replacement and Stop,
optional audiovisual failures, voice-listening gates, ambient-event expiry,
mission outcomes, and transport cancellation. The browser gallery is checked
for playable audio/video, no autoplay, exclusive playback, Stop, and mobile fit.

Validation completed on 2026-10-07: the action/transport/speech and emotion host
suites passed, all 157 Zumo desktop tests passed, and all 16 ambient-event
scenarios passed. All 17 gallery videos decoded successfully; desktop and mobile
playback controls were checked. The full `xiao_esp32s3_sense_idf` build and bundle
packaging passed with one compiler job (`-j 1`). Reported static RAM usage is
84,736 / 327,680 bytes (25.9%); application flash usage is 3,564,223 / 3,584,000
bytes (99.4%), leaving 19,777 bytes in the current application partition. This
is application-partition headroom, not total ESP32 storage. Neither controller
was installed or moved during this validation.

Any later installation must update the XIAO and the Zumo face engine,
`reaction_catalog.py`, wake reaction, and UART controller together. Preserve
backups and verify installed hashes/catalog identity. Stationary speaker/screen
and live command recognition checks precede supervised movement checks. Build
and desktop success do not establish physical sound quality, display smoothness,
braking, or turn accuracy.
