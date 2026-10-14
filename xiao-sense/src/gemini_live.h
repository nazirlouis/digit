#pragma once
#include <cstddef>
#include <cstdint>
#include "robot_actions.h"
#include "gemini_prompt.h"

namespace gemini_live {
enum class Event { Connecting, Connected, InputText, OutputText, Interrupted, TurnComplete, Error,
                   UserActivity, HeartbeatSent, GenerationComplete, InputReady, UserPrompt, RecoverySent,
                   ConnectionProgress, Retrying };
enum class ActionResult { Rejected, TestOnly, Queued, Busy };
struct Callbacks {
  void* context;
  bool (*cancelled)(void*);
  size_t (*readAudio)(int16_t*,size_t,void*)=nullptr;
  void (*event)(Event, const char*, void*);
  // Queue only. The main loop dispatches and reports the actual final outcome.
  ActionResult (*action)(const char* callId, const robot_actions::ToolRequest&, void*);
  size_t (*readVideo)(uint8_t*, size_t, uint32_t* frameSequence, void*) = nullptr;
  void (*videoSent)(const uint8_t* jpeg, size_t bytes, uint32_t frameSequence, void*) = nullptr;
  void (*cancelTool)(const char* callId, void*) = nullptr;
  bool (*pollToolResult)(char* callId, size_t idCapacity, char* result,
                         size_t resultCapacity, void*) = nullptr;
  bool (*canResume)(void*) = nullptr;
  // Zero keeps the established connection private/idle; nonzero identifies a
  // robot session. cancelled shuts down the worker, not this media epoch.
  uint32_t (*mediaEpoch)(void*) = nullptr;
  bool (*taskActive)(void*) = nullptr;
  // One bounded, locally authorized message; never read during probes/idle.
  size_t (*readText)(char*, size_t, void*) = nullptr;
  // Locally authorized continuation after connection recovery. This never
  // emits UserPrompt or starts/renews a task.
  size_t (*readRecovery)(char*, size_t, void*) = nullptr;
  size_t (*readHeartbeat)(char*, size_t, void*) = nullptr;
  // Diagnostics only, including rejected calls which never reach dispatch.
  void (*toolEvent)(const char* callId, const char* name, const robot_actions::ToolRequest&,
                    const char* status, const char* result, void*) = nullptr;
};
inline constexpr size_t kMaxTextLength = 1024;
// One worker owns the persistent socket when mediaEpoch is provided. Paused
// conversations send no camera data and reject server actions.
// Failed media and actions are never replayed during bounded-backoff reconnect.
// probe connects and validates setup without images or actions.
void run(const char* key, const char* model, const Callbacks& callbacks, bool probe,
         const char* systemInstructions = nullptr);
}  // namespace gemini_live
