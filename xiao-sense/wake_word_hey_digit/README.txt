# Hey Digit — microWakeWord model
Downloaded from https://microwakeword.com

## What's in this ZIP
  hey_digit.tflite    — TFLite model (the trained wake word)
  hey_digit.json      — ESPHome manifest (points at the .tflite)

Keep both files together. The manifest references the .tflite
by relative path (./hey_digit.tflite); renaming or separating them
will break the model.

## How to use it with ESPHome
1. Copy both files into the same folder as your ESPHome device
   YAML (e.g. ~/esphome/).
2. Add this snippet to your existing device YAML:

       micro_wake_word:
         models:
           - model: hey_digit.json
         on_wake_word_detected:
           - logger.log: "Wake word detected!"

3. Recompile and flash: esphome run your_device.yaml

Requires ESPHome 2024.7.0 or newer. Your microphone must be
configured at 16 kHz (the default for voice_assistant and most
ESP32 mic setups).

## Tuning
Open hey_digit.json and edit the "micro" section:

  probability_cutoff           Lower = more sensitive (more false
                               accepts). Higher = stricter (may miss
                               real triggers). Current: 0.5.
                               Try values in the 0.90–0.99 range.

  sliding_window_size          Frames averaged before deciding.
                               Bigger = smoother but slower to react.
                               Current: 5.

Leave feature_step_size and tensor_arena_size as delivered — they
match how this model was trained; changing them will break it.

## Help
  ESPHome docs: https://esphome.io/components/micro_wake_word/
  This service: https://microwakeword.com
