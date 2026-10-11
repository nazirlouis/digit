#pragma once
#include <cstddef>
#include <cstdint>
#include "robot_actions.h"

namespace gemini_live {
enum class Event { Connecting, Connected, InputText, OutputText, Interrupted, TurnComplete, Error,
                   UserActivity, GenerationComplete, InputReady };
enum class ActionResult { Rejected, TestOnly, Queued };
struct Callbacks {
  void* context;
  bool (*cancelled)(void*);
  size_t (*readAudio)(int16_t*, size_t, void*);
  bool (*playAudio)(const int16_t*, size_t, void*);
  void (*event)(Event, const char*, void*);
  // Queue only. The main loop dispatches and reports the actual final outcome.
  ActionResult (*action)(const char* callId, const robot_actions::ToolRequest&, void*);
  size_t (*readVideo)(uint8_t*, size_t, uint32_t* frameSequence, void*) = nullptr;
  void (*videoSent)(size_t bytes, uint32_t frameSequence, void*) = nullptr;
  void (*cancelTool)(const char* callId, void*) = nullptr;
  bool (*pollToolResult)(char* callId, size_t idCapacity, char* result,
                         size_t resultCapacity, void*) = nullptr;
  bool (*canResume)(void*) = nullptr;
  // Zero keeps the established connection private/idle; nonzero identifies a
  // wake conversation. cancelled shuts down the worker, not this media epoch.
  uint32_t (*mediaEpoch)(void*) = nullptr;
  bool (*taskActive)(void*) = nullptr;
};
// One worker owns the persistent socket when mediaEpoch is provided. Paused
// conversations send no microphone/camera data and discard server playback.
// Failed media and actions are never replayed during bounded-backoff reconnect.
// probe connects and validates setup without reading any microphone audio.
void run(const char* key, const char* model, const Callbacks& callbacks, bool probe);
}  // namespace gemini_live
