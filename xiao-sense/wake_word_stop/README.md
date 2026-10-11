# Local Stop keyword model

The unmodified `stop.tflite` and `stop.json` assets are from Kevin Ahrendt's
[microWakeWord Stop release](https://github.com/kahrendt/microWakeWord/releases/tag/stop),
named **"Stop" model beta 20241017.5**. The manifest format is version 2,
English, with probability cutoff 0.5, a five-score window, 10 ms feature step,
and upstream tensor arena recommendation of 21,000 bytes.

Retrieved on 2026-10-08. At retrieval the `stop` tag resolved to commit
`4665173cd35f1cff9a61e06fc427f124766c488e`. `release.json` preserves the release
metadata, including the asset digest published by GitHub. Both local asset
hashes match those published digests:

| File | SHA-256 |
| --- | --- |
| stop.tflite (45,744 bytes) | `b5a18c4ad681a89950dfade31011e1631bdcb333e93c84519a1a63ff4f071146` |
| stop.json | `bd13aeb1b83852649dc4fb6135cb160ff68716d14612b06f6a405342c57447aa` |

Source assets: [model](https://github.com/kahrendt/microWakeWord/releases/download/stop/stop.tflite),
[manifest](https://github.com/kahrendt/microWakeWord/releases/download/stop/stop.json).
The upstream repository's [Apache 2.0 license at that commit](https://github.com/kahrendt/microWakeWord/blob/4665173cd35f1cff9a61e06fc427f124766c488e/LICENSE)
is included as `LICENSE`; no separate model license or NOTICE accompanied these
release assets. The model and manifest have not been modified.

Digit embeds this model in the application, independently of the Digit wake
model, with its own frontend, inference arena, queue, and state. A detection
can only cancel active robot work through the existing local Stop path. No
wake word, Gemini key, internet request, or MultiNet runtime is involved.

The five-score threshold follows the manifest. The detector warms up on 100
feature frames after a session/reset, uses the ReSpeaker right ASR channel
without added gain, and shares the compute lock with wake/vision inference.
It keeps listening during robot sound so Stop is not intentionally masked;
ReSpeaker hardware echo cancellation supplies the existing echo reference.
These choices require acoustic acceptance on Digit, especially with motors,
speaker playback, and vision active. Host cancellation tests and a firmware
build do not establish detection accuracy or worst-case stopping latency.
