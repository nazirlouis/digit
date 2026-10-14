#include "gemini_live.h"
#include "camera_frame_limits.h"
#include "robot_actions.h"
#include "task_policy.h"
#ifndef GEMINI_LIVE_HOST_TEST
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace {
constexpr size_t kMaxMessage = 128 * 1024;
constexpr size_t kTextBufferBytes = gemini_live::kMaxTextLength + 1;
constexpr size_t kMaxVideo = camera_frame_limits::kMaxJpegBytes;
constexpr size_t kOutgoingBytes = 4 * ((kMaxVideo + 2) / 3) + 128;
constexpr uint32_t kVideoIntervalMs = 1000;
constexpr uint32_t kPauseDrainMs = 5000;
// Keep individual WebSocket writes below one 4 KiB TLS output record. IDF's
// raw frame writer cannot finish a partially written payload for us.
constexpr size_t kWriteFragmentBytes = 2048;
constexpr uint32_t kSetupWriteMs = 10000;
uint32_t clockMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
uint32_t mediaEpoch(const gemini_live::Callbacks& cb) { return cb.mediaEpoch ? cb.mediaEpoch(cb.context) : 1; }
bool localSpeechEvidence(const int16_t* samples,size_t count) {
  if(!samples||count<160)return false;
  uint64_t energy=0;size_t audible=0;int peak=0;
  for(size_t i=0;i<count;++i){const int value=std::abs(int(samples[i]));energy+=uint64_t(value)*value;peak=std::max(peak,value);audible+=value>=200;}
  return peak>=500&&audible>=(count+15)/16&&energy>=uint64_t(count)*180*180;
}
const cJSON* field(const cJSON* object, const char* name) { return cJSON_GetObjectItemCaseSensitive(object, name); }
const char* text(const cJSON* object) { return cJSON_IsString(object) ? object->valuestring : ""; }
bool boundedJson(const char* data, size_t size) {
  unsigned depth = 0;
  unsigned structuralTokens = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    if (!c) return false;
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 12 || ++structuralTokens > 256) return false; }
    else if (c == ',' || c == ':') { if (++structuralTokens > 256) return false; }
    else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
  }
  return !depth && !quoted && !strstr(data, "\\u0000");
}
int actionId(const char* name) {
  for (const auto& definition : robot_actions::definitions)
    if (!strcmp(name, definition.toolName)) return static_cast<int>(definition.command);
  return 0;
}
bool uniqueFields(const cJSON* object) {
  if (!cJSON_IsObject(object)) return false;
  for (auto* a = object->child; a; a = a->next)
    for (auto* b = a->next; b; b = b->next)
      if (!a->string || !b->string || !strcmp(a->string, b->string)) return false;
  return true;
}
bool integer(const cJSON* args, const char* key, int low, int high, int& result) {
  const auto* n=field(args,key);
  if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) || n->valuedouble<low || n->valuedouble>high || n->valuedouble!=int(n->valuedouble)) return false;
  result=int(n->valuedouble); return true;
}
bool stringArg(const cJSON* args, const char* key, char* result, size_t capacity) {
  const auto* n=field(args,key); const char* value=text(n);
  if (!cJSON_IsString(n) || !*value || strlen(value)>=capacity) return false;
  snprintf(result,capacity,"%s",value); return true;
}
bool parseTool(const char* name, const cJSON* args, robot_actions::ToolRequest& r) {
  using C=robot_actions::Command;
  r.command=static_cast<C>(actionId(name));
  if (r.command==C::None || !uniqueFields(args)) return false;
  const auto* rationale = field(args,"action_reason");
  if (rationale && (!stringArg(args,"action_reason",r.actionReason,sizeof(r.actionReason)) || !gemini_live::validSystemInstructions(r.actionReason))) return false;
  const int count=cJSON_GetArraySize(args) - (rationale ? 1 : 0);
  if (r.command==C::Stop || r.command==C::Observe || r.command==C::Dance) return count==0;
  if (r.command==C::StartTask) return count==1 && stringArg(args,"goal",r.goal,sizeof(r.goal));
  if (r.command==C::TurnLeft || r.command==C::TurnRight) return count==1 && integer(args,"degrees",1,90,r.amount);
  if (r.command==C::Forward || r.command==C::Backward || r.command==C::Push)
    return count==1 && integer(args,"distance_cm",1,r.command==C::Backward?10:30,r.amount);
  if (r.command==C::Expression) {
    if (count!=2 || !stringArg(args,"expression",r.expression,sizeof(r.expression)) || !integer(args,"duration_ms",500,10000,r.durationMs)) return false;
    const char* names[]={"NEUTRAL","HAPPY","SAD","MAD","SERIOUS","CURIOUS","SURPRISED","SLEEPING","CONFUSED","SLEEPY","EXCITED","CRYING","LOVE"};
    for (const char* face:names) if (!strcmp(face,r.expression)) return true;
    return false;
  }
  return r.command==C::FinishTask && count==2 && stringArg(args,"outcome",r.outcome,sizeof(r.outcome)) &&
    (!strcmp(r.outcome,"completed") || !strcmp(r.outcome,"failed") || !strcmp(r.outcome,"cancelled")) && stringArg(args,"reason",r.reason,sizeof(r.reason));
}
struct Json {
  cJSON* value = nullptr;
  ~Json() { cJSON_Delete(value); }
};
struct Socket {
  esp_transport_handle_t tls = nullptr, ws = nullptr;
  ~Socket() {
    if (ws) { esp_transport_close(ws); esp_transport_destroy(ws); }
    if (tls) esp_transport_destroy(tls);
  }
};
struct Buffers {
  char* message = static_cast<char*>(heap_caps_malloc(kMaxMessage + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  unsigned char* scratch = static_cast<unsigned char*>(heap_caps_malloc(kTextBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  char* outgoing = static_cast<char*>(heap_caps_malloc(kOutgoingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* video = nullptr;
  ~Buffers() {
    if (message) { mbedtls_platform_zeroize(message, kMaxMessage + 1); heap_caps_free(message); }
    if (scratch) { mbedtls_platform_zeroize(scratch, kTextBufferBytes); heap_caps_free(scratch); }
    if (outgoing) { mbedtls_platform_zeroize(outgoing, kOutgoingBytes); heap_caps_free(outgoing); }
    if (video) { mbedtls_platform_zeroize(video, kMaxVideo); heap_caps_free(video); }
  }
};
// ESP-IDF masks outgoing frames in place, including PING/PONG payloads.
struct WriteFailure { const char* reason = "preparing setup"; size_t offset = 0, expected = 0; int written = 0; };
bool send(esp_transport_handle_t ws, char* data, const gemini_live::Callbacks& cb,
          uint32_t expectedEpoch = 0, uint32_t timeoutMs = 1500, WriteFailure* failure = nullptr) {
  if (cb.cancelled(cb.context)) {
    if (failure) *failure = {"write cancelled before start", 0, 0, 0};
    return false;
  }
  const size_t length = strlen(data);
  // Encoding may outlast Stop. Omit obsolete unsent media without closing the
  // still-valid warm connection; a write already in progress may finish.
  if (expectedEpoch && mediaEpoch(cb) != expectedEpoch) return true;
  const uint32_t started = clockMs();
  size_t offset = 0;
  do {
    const uint32_t elapsed = clockMs() - started;
    if (cb.cancelled(cb.context) || elapsed >= timeoutMs) {
      if (failure) *failure = {"write cancelled or timed out", offset, length, 0};
      return false;
    }
    // Once a fragmented message starts, closing the socket is the only safe
    // cancellation: never finish it with obsolete camera data.
    if (expectedEpoch && mediaEpoch(cb) != expectedEpoch) {
      if (offset && failure) *failure = {"media session ended during write", offset, length, 0};
      return offset == 0;
    }
    const size_t count = std::min(kWriteFragmentBytes, length - offset);
    const int opcode = (offset ? WS_TRANSPORT_OPCODES_CONT : WS_TRANSPORT_OPCODES_TEXT) |
        (offset + count == length ? WS_TRANSPORT_OPCODES_FIN : 0);
    const int waitMs = static_cast<int>(std::min<uint32_t>(500, timeoutMs - elapsed));
    const int written = esp_transport_ws_send_raw(ws, static_cast<ws_transport_opcodes_t>(opcode),
        data + offset, static_cast<int>(count), waitMs);
    if (written != static_cast<int>(count)) {
      if (failure) *failure = {"transport write", offset, count, written};
      return false;
    }
    offset += count;
  } while (offset < length);
  return true;
}
bool sendJson(esp_transport_handle_t ws, cJSON* object, const gemini_live::Callbacks& cb,
              uint32_t expectedEpoch = 0, uint32_t timeoutMs = 1500, WriteFailure* failure = nullptr) {
  char* data = cJSON_PrintUnformatted(object);
  if (!data && failure) failure->reason = "encoding setup";
  const bool ok = data && send(ws, data, cb, expectedEpoch, timeoutMs, failure);
  cJSON_free(data);
  return ok;
}
bool heartbeat(esp_transport_handle_t ws) {
  char payload[] = "digit";
  return esp_transport_ws_send_raw(ws, static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN),
      payload, sizeof(payload) - 1, 500) == sizeof(payload) - 1;
}
bool setup(esp_transport_handle_t ws, const char* model, const gemini_live::Callbacks& cb,
           const char* resume = "", const char* systemInstructions = nullptr, WriteFailure* failure = nullptr) {
  const char* instructions = systemInstructions && *systemInstructions ? systemInstructions : gemini_live::kDefaultSystemInstructions;
  if (strcmp(model, "gemini-robotics-er-2-streaming-preview") || !gemini_live::validSystemInstructions(instructions)) return false;
  Json root{cJSON_Parse(R"JSON({"setup":{
    "model":"models/gemini-robotics-er-2-streaming-preview",
    "generationConfig":{"responseModalities":["TEXT"],"maxOutputTokens":512,"mediaResolution":"MEDIA_RESOLUTION_LOW"},
    "inputAudioTranscription":{},
    "realtimeInputConfig":{"automaticActivityDetection":{"silenceDurationMs":700},"turnCoverage":"TURN_INCLUDES_AUDIO_ACTIVITY_AND_ALL_VIDEO"},
    "systemInstruction":{"parts":[{}]},
    "tools":[{"functionDeclarations": [{"name": "start_task", "description": "Optional compatibility tool: name the task already opened by the user prompt and request an observation. Never creates or restarts a task and never resets its limits. Prefer observe directly; start_task is not required.", "parameters": {"type": "OBJECT", "properties": {"goal": {"type": "STRING"}}, "required": ["goal"]}}, {"name": "observe", "description": "Request a fresh camera frame and compact local sensor status."}, {"name": "turn_left", "behavior":"NON_BLOCKING", "description": "Turn left using local gyro; result follows confirmed rest and new camera frame.", "parameters": {"type": "OBJECT", "properties": {"degrees": {"type": "INTEGER", "minimum": 1, "maximum": 90}}, "required": ["degrees"]}}, {"name": "turn_right", "behavior":"NON_BLOCKING", "description": "Turn right using local gyro; result follows confirmed rest and new camera frame.", "parameters": {"type": "OBJECT", "properties": {"degrees": {"type": "INTEGER", "minimum": 1, "maximum": 90}}, "required": ["degrees"]}}, {"name": "move_forward", "behavior":"NON_BLOCKING", "description": "Travel forward by measured encoder distance with local readiness checks.", "parameters": {"type": "OBJECT", "properties": {"distance_cm": {"type": "INTEGER", "minimum": 1, "maximum": 30}}, "required": ["distance_cm"]}}, {"name": "move_backward", "behavior":"NON_BLOCKING", "description": "Travel backward by measured encoder distance; front sensor cannot see behind.", "parameters": {"type": "OBJECT", "properties": {"distance_cm": {"type": "INTEGER", "minimum": 1, "maximum": 10}}, "required": ["distance_cm"]}}, {"name": "push_object", "behavior":"NON_BLOCKING", "description": "Intentional forward-only push of a lightweight play object, from rest. Uses maximum motor target and high acceleration. Distance is total track travel, including braking; only the close-obstacle threshold is bypassed for this job. Wait for rest and a fresh image to judge the object.", "parameters": {"type": "OBJECT", "properties": {"distance_cm": {"type": "INTEGER", "minimum": 1, "maximum": 30}}, "required": ["distance_cm"]}}, {"name": "set_expression", "description": "Set eyes for 500..10000 ms, with one small body gesture when idle and safe. During driving only eyes animate. Twelve emotions have original R2-D2 sounds when idle and safe; NEUTRAL is silent. SERIOUS and SLEEPING sound on visibility without movement; the other audible emotions sound on gesture start. Never generate speech. Never use for navigation.", "parameters": {"type": "OBJECT", "properties": {"expression": {"type": "STRING", "enum": ["NEUTRAL", "HAPPY", "SAD", "MAD", "SERIOUS", "CURIOUS", "SURPRISED", "SLEEPING", "CONFUSED", "SLEEPY", "EXCITED", "CRYING", "LOVE"]}, "duration_ms": {"type": "INTEGER", "minimum": 500, "maximum": 10000}}, "required": ["expression", "duration_ms"]}}, {"name": "dance", "description": "Perform one safe local side-to-side dance with Happy eyes, flashing RGB lights and an original electronic melody. Requires an active user task and a fresh observation. Takes about five seconds; Stop cancels everything. Do not use for navigation or claim restored heading. No arguments."}, {"name": "finish_task", "description": "Finish the whole task, distinct from a model response turn.", "parameters": {"type": "OBJECT", "properties": {"outcome": {"type": "STRING", "enum": ["completed", "failed", "cancelled"]}, "reason": {"type": "STRING"}}, "required": ["outcome", "reason"]}}, {"name": "stop", "description": "Immediately stop the robot and end the conversation."}]}],
    "contextWindowCompression":{"triggerTokens":"8192","slidingWindow":{"targetTokens":"4096"}},
    "sessionResumption":{}
  }})JSON")};
  if (!root.value) return false;
  auto* config = cJSON_GetObjectItemCaseSensitive(root.value, "setup");
  // ER 2 requires BLOCKING tools. Local Stop preempts independently.
  // Navigation, observation and face acknowledgements must return their actual
  // outcome before the model plans another step. The network/control loops
  // remain asynchronous; local Stop and a received stop() still preempt them.
  auto* declarations = field(cJSON_GetArrayItem(field(config,"tools"),0),"functionDeclarations");
  // Optional for compatibility and emergency Stop; the policy asks for it on
  // ordinary calls. It is descriptive metadata, never execution authority.
  cJSON* declaration = nullptr;
  cJSON_ArrayForEach(declaration,declarations) {
    auto* parameters=cJSON_GetObjectItemCaseSensitive(declaration,"parameters");
    if (!parameters) {
      parameters=cJSON_AddObjectToObject(declaration,"parameters");
      cJSON_AddStringToObject(parameters,"type","OBJECT");
    }
    auto* properties=cJSON_GetObjectItemCaseSensitive(parameters,"properties");
    if (!properties) properties=cJSON_AddObjectToObject(parameters,"properties");
    auto* reason=cJSON_AddObjectToObject(properties,"action_reason");
    cJSON_AddStringToObject(reason,"type","STRING");
    cJSON_AddStringToObject(reason,"description","One short public sentence explaining the observation or goal behind this action; at most 191 UTF-8 bytes. Never private internal reasoning. Stop never needs a reason.");
  }
  cJSON_ArrayForEach(declaration,declarations) {
    cJSON_DeleteItemFromObjectCaseSensitive(declaration,"behavior");
    if (!cJSON_AddStringToObject(declaration,"behavior","BLOCKING")) return false;
  }
  auto* part = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(config, "systemInstruction"), "parts"), 0);
  if (!cJSON_AddStringToObject(part, "text", instructions)) return false;
  auto* parts = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(config, "systemInstruction"), "parts");
  Json policy{cJSON_CreateObject()};
  char limits[768];
  snprintf(limits, sizeof(limits), "Local firmware policy: the current maximum is %u movements per task and %lu seconds total. Each fresh spoken prompt or dashboard typed message automatically opens a local task if none is active. New input steers an active task without resetting its limits. Do not call start_task to begin. Reply-only tasks may finish without observing or moving; call finish_task after answering. Reply turns and tool results never start or finish a task. Once finished or failed, wait for fresh user input. Controller observation prompts never start or restart a task. A started session can keep observing and expressing emotions after finish_task. This lifecycle overrides older saved instructions. Local Stop and movement safeguards always apply.",
      robot_actions::TaskPolicy::maxMoves, static_cast<unsigned long>(robot_actions::TaskPolicy::deadlineMs / 1000));
  if (!policy.value || !cJSON_AddStringToObject(policy.value, "text", limits) || !cJSON_AddItemToArray(parts, policy.value)) return false;
  policy.value = nullptr;
  Json recoveryPolicy{cJSON_CreateObject()};
  if (!recoveryPolicy.value || !cJSON_AddStringToObject(recoveryPolicy.value,"text",
      "Tool sequencing: wait for the matching function response before requesting the next tool. "
      "Call observe before the first movement of an automatically opened task. No camera is needed for a reply-only task. The optional start_task compatibility call also returns an observation; never immediately repeat it with observe. "
      "A camera frame alone is not the function response; wait for the matching sensor/result response. "
      "busy_waiting_for_tool means the earlier tool is still pending, not failed or cancelled. "
      "Do not retry or call stop merely because a tool is pending; continue after its actual result. "
      "Use finish_task with a reason for normal completion or inability to continue. "
      "stop is immediate global cancellation for a user Stop or an urgent safety need. "
      "Visual navigation and recovery policy (applies with any personality instructions): "
      "If an object blocks your way during an active task, use the latest camera frame and sensors to figure out how to avoid it, "
      "then continue the same task without waiting for another user request. "
      "Choose your own route, turn angles and movement distances based on what you observe, within the tool limits and local safety controls. "
      "An obstacle alone does not mean the task is completed. If you cannot find a safe route, finish_task as failed and explain why. "
      "Zumo owns bounded safety recovery. A paused or recovering tool remains pending: wait for its terminal result, "
      "never issue your own recovery reverse or another tool while it is pending. Brief sensor/packet retries remain local. "
      "A terminal result follows confirmed rest and a fresh camera frame, with failure_reason, recovery_cause, "
      "recovery_attempts, requested_mm and encoder-measured travelled_mm. These are track estimates, not proof of ground displacement; "
      "TRACK_MISMATCH or a stall can mean obstruction or slip and does not prove contact. "
      "position_known=false means movement estimates are uncertain; observe and replan without assuming the requested distance completed. "
      "front_distance_text >1 metre and front_clear_beyond_range=true mean greater than one metre, clear to continue subject to other safeguards. "
      "A fresh >1 metre reading is not a sensor failure and must not cause a stationary abort; missing updates and actual errors remain distinct. "
      "blocked_recovered, obstacle_stopped, push_blocked and edge_retreated mean the original movement was interrupted. "
      "If ready and task_active are true, inspect the new image, choose a different heading and continue the same task. "
      "Never repeat a retreat already performed by firmware or assume that the opposite direction is safe: the front sensor cannot see behind. "
      "There is no cumulative recovery-count limit; successful recoveries do not by themselves end a task. "
      "The original task deadline and move budget still apply. Edge recovery does not restart them. "
      "When detour_pending is true, backing up or turning has not restored forward progress; choose a safe detour or finish as failed. "
      "Local limits also stop repeated lack of progress and six detour moves without forward progress. "
      "When an edge is detected, the Zumo automatically cuts the interrupted motion and reverses 100 mm (10 cm). "
      "edge_retreated means that the 10 cm encoder-bounded reverse has finished and stopped tracks were confirmed. "
      "The local edge reverse does not wait for IMU stability or further edge readings; "
      "do not repeat the retreat. The response includes edge_sensor_mask (bit 0 is leftmost), "
      "retreated_mm, net travelled_mm including the retreat, and a fresh post-retreat image. "
      "The edge was in the direction you were approaching. Turn to a different heading before moving forward. "
      "If task_active is true, choose a different route from the new observation "
      "and continue the same goal without asking the user to repeat it. Never claim the original move finished. "
      "Individual recovery safeguards, failed reverse, unrecovered edge, persistent sensor failure or user Stop cannot be bypassed. "
      "Intentional object interaction: during an active task you may choose push_object(distance_cm) for a suitable lightweight play object. "
      "This permission overrides an older blanket instruction never to push objects, but preserves any user-stated boundary. "
      "Use a fresh observation, line up while stationary, and choose an integer 1 to 30 cm of total forward travel. "
      "Push uses maximum motor target and high acceleration, with a temporary close-obstacle exception only for that job; "
      "all other local safety checks and Stop still apply. Ordinary navigation must avoid contact. "
      "push_completed confirms bounded track movement only; inspect the new image before claiming an object fell over. "
      "A push cannot by itself clear a navigation detour, and a blocked push must not be automatically repeated or made harder.") ||
      !cJSON_AddItemToArray(parts,recoveryPolicy.value)) return false;
  recoveryPolicy.value=nullptr;
  // Session capabilities override older saved speech instructions.
  Json welcome{cJSON_CreateObject()};
  if (!welcome.value || !cJSON_AddStringToObject(welcome.value,"text",gemini_live::kNewChatContext) ||
      !cJSON_AddItemToArray(parts,welcome.value)) return false;
  welcome.value=nullptr;
  Json dashboardText{cJSON_CreateObject()};
  if (!dashboardText.value || !cJSON_AddStringToObject(dashboardText.value,"text",gemini_live::kDashboardTextInstructions) ||
      !cJSON_AddItemToArray(parts,dashboardText.value)) return false;
  dashboardText.value=nullptr;
  if (*resume && !cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(config, "sessionResumption"), "handle", resume)) return false;
  return sendJson(ws, root.value, cb, 0, kSetupWriteMs, failure);
}
struct Calls {
  struct Entry { char id[129]; char name[32]; bool pending; uint32_t epoch; } entries[128]{};
  size_t count = 0;
  int index(const char* id) const {
    for (size_t i = 0; i < count; ++i) if (!strcmp(entries[i].id, id)) return static_cast<int>(i);
    return -1;
  }
  int remember(const char* id) {
    if (!*id || strlen(id) >= sizeof(entries[0].id) || count >= 128) return -1;
    if (index(id) >= 0) return -1;
    strcpy(entries[count].id, id);
    return static_cast<int>(count++);
  }
  bool pending() const {
    for (size_t i = 0; i < count; ++i) if (entries[i].pending) return true;
    return false;
  }
  void cancel(const char* id, const gemini_live::Callbacks& cb) {
    const int found = index(id);
    if (found < 0 || !entries[found].pending) return;
    entries[found].pending = false;
    if (cb.cancelTool) cb.cancelTool(id, cb.context);
  }
  void cancelAll(const gemini_live::Callbacks& cb) {
    for (size_t i = 0; i < count; ++i)
      if (entries[i].pending) cancel(entries[i].id, cb);
  }
};
struct State {
  Calls calls;
  char resume[2048]{};
  bool resumable=false, modelBusy=false, awaitingTurn=false, goAway=false;
  bool userSpeaking=false,preVadSpeech=false;uint32_t preVadAt=0;
  uint32_t goAwayAt=0, goAwayWaitMs=0;
  bool activitySeen=false, transcriptSeen=false, permitConsumed=false;
  uint32_t userEvidenceAt=0;
  uint32_t epoch=1, drainingAt=0;
  bool inputReady=true, inputSent=false, draining=false, epochHadMedia=false;
  void clearEvidence() { activitySeen=transcriptSeen=false; permitConsumed=true; userEvidenceAt=0; }
  bool outputAllowed() const { return epoch && inputReady && inputSent; }
  void startActivity() { activitySeen=true; transcriptSeen=permitConsumed=false; userEvidenceAt=clockMs(); }
  bool transcript() { const bool fresh=activitySeen&&!transcriptSeen; if(activitySeen) transcriptSeen=true; return fresh; }
  bool authorize(robot_actions::Command command, bool task) {
    if(!epoch||!inputReady||!inputSent) return false;
    using C=robot_actions::Command;
    if(command==C::Stop) return true;
    if(robot_actions::isMovement(command)||command==C::FinishTask||command==C::StartTask) return task;
    return command==C::Expression||command==C::Observe;
  }
  bool canResume() const { return goAway&&resumable&&resume[0]&&!modelBusy&&!awaitingTurn&&!userSpeaking&&!preVadSpeech&&!calls.pending()&&!draining; }
};
struct StateStorage {
  State* value = static_cast<State*>(heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const gemini_live::Callbacks& cb;
  ~StateStorage() { if (value) { value->calls.cancelAll(cb); mbedtls_platform_zeroize(value, sizeof(State)); heap_caps_free(value); } }
};
bool resultReply(esp_transport_handle_t ws, const char* id, const char* name, const char* result, const gemini_live::Callbacks& cb,
                 uint32_t expectedEpoch = 0, const char* scheduling = "INTERRUPT") {
  Json reply{cJSON_CreateObject()};
  if (!reply.value) return false;
  auto* tool = cJSON_AddObjectToObject(reply.value, "toolResponse");
  auto* responses = tool ? cJSON_AddArrayToObject(tool, "functionResponses") : nullptr;
  auto* item = cJSON_CreateObject();
  if (!responses || !item) { cJSON_Delete(item); return false; }
  if (!cJSON_AddItemToArray(responses, item)) { cJSON_Delete(item); return false; }
  auto* value = cJSON_Parse(result);
  if (!cJSON_IsObject(value)) { cJSON_Delete(value); value=cJSON_CreateObject(); if (value) cJSON_AddStringToObject(value,"result",result); }
  (void)scheduling;  // ER 2 supports blocking tools only; no async scheduling flags.
  if (value) cJSON_AddItemToObject(item,"response",value);
  if (!value || !cJSON_AddStringToObject(item, "id", id) || !cJSON_AddStringToObject(item, "name", name)) return false;
  return sendJson(ws, reply.value, cb, expectedEpoch);
}
bool toolReply(esp_transport_handle_t ws, const cJSON* call, Calls& seen, const gemini_live::Callbacks& cb, State* evidence = nullptr,
               bool allow = true) {
  const auto* calls = field(call, "functionCalls");
  const int count = cJSON_GetArraySize(calls);
  if (!uniqueFields(call) || !cJSON_IsArray(calls) || count < 1 || count > 8) return false;
  const cJSON* entry = nullptr;
  cJSON_ArrayForEach(entry, calls) {
    if (cb.cancelled(cb.context)) return false;
    const char* id = text(field(entry, "id"));
    const char* name = text(field(entry, "name"));
    if (!*id || strlen(id) > 128 || !*name || strlen(name) > 64) return false;
    // Duplicate calls never cause execution or a second response, even after resumption.
    if (seen.index(id) >= 0) continue;
    const int slot = seen.remember(id);
    if (slot < 0) {
      // A lifetime dedup table must fail closed rather than evict an old ID
      // that the server could replay after context compression/resumption.
      if (cb.toolEvent) {
        robot_actions::ToolRequest rejected;
        cb.toolEvent(id,name,rejected,"Requested","",cb.context);
        cb.toolEvent(id,name,rejected,"Rejected","Function-call deduplication history is full; no action started",cb.context);
      }
      if (!resultReply(ws, id, name, "rejected_action_history_full", cb)) return false;
      continue;
    }
    const auto* args = field(entry, "args");
    Json empty{args ? nullptr : cJSON_CreateObject()};
    robot_actions::ToolRequest request;
    const bool valid = count == 1 && uniqueFields(entry) && ((args && cJSON_GetArraySize(entry)==3) || (!args && cJSON_GetArraySize(entry)==2)) && parseTool(name,args?args:empty.value,request);
    if (cb.toolEvent) cb.toolEvent(id,name,request,"Requested","",cb.context);
    const bool task = cb.taskActive && cb.taskActive(cb.context);
    const bool authorized = allow && valid && (!evidence ||
        (mediaEpoch(cb) == evidence->epoch && evidence->authorize(request.command,task)));
    const auto result = authorized && cb.action ? cb.action(id, request, cb.context) : gemini_live::ActionResult::Rejected;
    if (cb.toolEvent) cb.toolEvent(id,name,request,
        result==gemini_live::ActionResult::Queued ? "Queued" : result==gemini_live::ActionResult::Busy ? "Blocked" : result==gemini_live::ActionResult::TestOnly ? "Test only" : "Rejected",
        result==gemini_live::ActionResult::Queued ? "Awaiting local checks" : result==gemini_live::ActionResult::Busy ? "Waiting for the previous tool result; no action started" : result==gemini_live::ActionResult::TestOnly ? "No action started" : !valid ? "Invalid or unsupported function arguments; no action started" : "Session or authorization checks rejected this call; no action started",cb.context);
    snprintf(seen.entries[slot].name,sizeof(seen.entries[slot].name),"%s",name);
    if (result == gemini_live::ActionResult::Queued) {
      seen.entries[slot].pending = true;
      seen.entries[slot].epoch = evidence ? evidence->epoch : mediaEpoch(cb);
      continue;  // Only the actual main-loop outcome answers this function call.
    }
    if (result == gemini_live::ActionResult::Busy) {
      // Do not interrupt the outstanding tool with an ambiguous rejection and
      // provoke more retries. Its real completion remains pending and will
      // resume planning with INTERRUPT when the main loop supplies the result.
      if (!resultReply(ws,id,name,
          R"({"result":"busy_waiting_for_tool","action_started":false,"previous_tool_pending":true,"instruction":"Wait for the previous tool's function response. Do not retry or call stop because it is pending; continue the same task after its actual result."})",
          cb,evidence ? evidence->epoch : 0,"SILENT")) return false;
      continue;
    }
    if (!resultReply(ws, id, name, result == gemini_live::ActionResult::TestOnly ?
        "test_only_no_action_started" : "rejected_no_action_started", cb)) return false;
  }
  return true;
}
bool flushToolResults(esp_transport_handle_t ws, State& state, const gemini_live::Callbacks& cb) {
  if (!cb.pollToolResult) return true;
  char id[129]{}, result[1536]{};
  // Bound callback draining so camera upload cannot starve.
  for (unsigned i = 0; i < 4 && !cb.cancelled(cb.context); ++i) {
    if (!cb.pollToolResult(id, sizeof(id), result, sizeof(result), cb.context)) break;
    if (mediaEpoch(cb) != state.epoch) return true;
    if (!memchr(id, 0, sizeof(id)) || !memchr(result, 0, sizeof(result))) return false;
    const int found = state.calls.index(id);
    if (found < 0 || !state.calls.entries[found].pending || !state.epoch ||
        mediaEpoch(cb) != state.epoch || state.calls.entries[found].epoch != state.epoch) continue;
    state.resumable = false;
    if (!resultReply(ws, id, state.calls.entries[found].name, result, cb, state.epoch)) return false;
    state.calls.entries[found].pending = false;
    state.modelBusy = true;  // Wait for the response turn or fresh idle indication.
  }
  return true;
}
bool media(esp_transport_handle_t ws, char* output, size_t capacity, const char* kind, const char* mime,
           const uint8_t* data, size_t bytes, const gemini_live::Callbacks& cb, uint32_t expectedEpoch = 0,
           WriteFailure* failure = nullptr) {
  if (failure) failure->reason = "encoding media";
  const int prefix = snprintf(output, capacity, "{\"realtimeInput\":{\"%s\":{\"mimeType\":\"%s\",\"data\":\"", kind, mime);
  if (prefix <= 0 || static_cast<size_t>(prefix) + 6 >= capacity) return false;
  size_t encoded = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output + prefix), capacity - prefix - 6,
      &encoded, data, bytes) != 0) return false;
  memcpy(output + prefix + encoded, "\"}}}", 5);
  return send(ws, output, cb, expectedEpoch, 1500, failure);
}
void updateResumption(State& state, const cJSON* update) {
  if (!update) return;
  const char* handle = text(field(update, "newHandle"));
  state.resumable = cJSON_IsTrue(field(update, "resumable")) && *handle && strlen(handle) < sizeof(state.resume);
  if (state.resumable) strcpy(state.resume, handle);
}
void updateGoAway(State& state, const cJSON* notice) {
  if (!notice) return;
  const char* duration = text(field(notice, "timeLeft"));
  char* end = nullptr;
  const double seconds = strtod(duration, &end);
  state.goAway = true;
  state.goAwayAt = clockMs();
  // Invalid/missing deadlines end the connection safely rather than guessing.
  state.goAwayWaitMs = end && *end == 's' && end[1] == 0 && seconds > 0 && seconds <= 600 ?
      static_cast<uint32_t>(seconds * 1000) : 0;
}
}  // namespace

namespace gemini_live {
void run(const char* key, const char* model, const Callbacks& cb, bool probe, const char* systemInstructions) {
  if (!cb.cancelled || !cb.event) return;
  const auto emit = [&](Event event, const char* value) {
    if (!cb.cancelled(cb.context)) cb.event(event, key && *key && strstr(value, key) ? "[credential omitted]" : value, cb.context);
  };
  // This IDF version logs the upgrade header even on an ERROR write failure.
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  esp_log_level_set("transport_base", ESP_LOG_WARN);
  Buffers buffers;
  StateStorage state{static_cast<State*>(heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), cb};
  if (!buffers.message || !buffers.scratch || !buffers.outgoing || !state.value) {
    emit(Event::Error, "Not enough memory for Gemini Live"); return;
  }
  if (!key || !*key || !model || strcmp(model, "gemini-robotics-er-2-streaming-preview")) {
    emit(Event::Error, "Invalid Live settings"); return;
  }
  if (!probe && cb.readVideo) {
    buffers.video = static_cast<uint8_t*>(heap_caps_malloc(kMaxVideo, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffers.video) { emit(Event::Error, "Not enough memory for Live camera streaming"); return; }
  }
  const bool persistent = !probe && cb.mediaEpoch;
  state.value->epoch = mediaEpoch(cb);
  state.value->inputReady = !persistent;
  uint32_t retryMs = 1000;
  uint32_t lastVideoUpload = clockMs() - kVideoIntervalMs;
  for (;;) {
    if (cb.cancelled(cb.context)) return;
    emit(Event::Connecting, state.value->resume[0] ? "Resuming Gemini Live" : "Connecting to Gemini Live");
    const auto connection = [&]() -> bool {
    Socket socket;
    socket.tls = esp_transport_ssl_init();
    if (socket.tls) socket.ws = esp_transport_ws_init(socket.tls);
    if (!socket.ws) { emit(Event::Error, "Could not create Gemini Live connection"); return false; }
    esp_transport_ssl_crt_bundle_attach(socket.tls, esp_crt_bundle_attach);
    char headers[560];
    const int headerBytes = snprintf(headers, sizeof(headers), "x-goog-api-key: %s\r\n", key);
    esp_transport_ws_config_t config{};
    config.ws_path = "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
    config.headers = headers;
    config.propagate_control_frames = true;
    const auto configured = headerBytes > 0 && headerBytes < static_cast<int>(sizeof(headers)) ?
        esp_transport_ws_set_config(socket.ws, &config) : ESP_ERR_INVALID_ARG;
    mbedtls_platform_zeroize(headers, sizeof(headers));
    if (configured != ESP_OK) { emit(Event::Error, "Could not prepare Gemini Live connection"); return false; }
    if (cb.cancelled(cb.context)) return false;
    emit(Event::ConnectionProgress, "Opening secure Gemini connection");
    if (esp_transport_connect(socket.ws, "generativelanguage.googleapis.com", 443, 12000) < 0) {
      const int status = esp_transport_ws_get_upgrade_request_status(socket.ws);
      if (status == 401 || status == 403) emit(Event::Error, "Gemini refused the API key; check key permissions");
      else if (status == 429) emit(Event::Error, "Gemini connection rate limited; check quota");
      else if (status >= 500) emit(Event::Error, "Gemini service unavailable; retrying automatically");
      else emit(Event::Error, "Secure Gemini connection failed; check internet and clock sync");
      return false;
    }
    // IDF writes each WebSocket header and payload as separate TLS records.
    // Nagle/delayed ACK can otherwise stall JPEG fragments long enough to
    // overflow the live microphone queue even on a strong Wi-Fi connection.
    const int socketFd=esp_transport_get_socket(socket.tls), noDelay=1;
    if(socketFd<0 || setsockopt(socketFd,IPPROTO_TCP,TCP_NODELAY,&noDelay,sizeof(noDelay))!=0) {
      emit(Event::Error,"Could not configure low-latency Live connection"); return false;
    }
    emit(Event::ConnectionProgress, "Sending Gemini instructions and tools");
    WriteFailure failure;
    if (!setup(socket.ws, model, cb, state.value->resume, systemInstructions, &failure)) {
      char reason[128];
      snprintf(reason, sizeof(reason), "Could not send Live setup: %s %d/%u at byte %u", failure.reason,
          failure.written, static_cast<unsigned>(failure.expected), static_cast<unsigned>(failure.offset));
      emit(Event::Error, reason); return false;
    }
    emit(Event::ConnectionProgress, "Waiting for Gemini to confirm setup");
    // A resumed conversation does not renew permission for an older utterance.
    state.value->clearEvidence();
    state.value->epoch = mediaEpoch(cb);
    state.value->inputReady = !persistent;
    state.value->inputSent = state.value->epochHadMedia = state.value->draining = false;
    state.value->userSpeaking=state.value->preVadSpeech=false; state.value->modelBusy = state.value->awaitingTurn = false;
    state.value->goAway = false;
    state.value->resumable = false;
    const uint32_t started = clockMs();
    uint32_t lastReceived = started, lastPing = started, lastVideo = lastVideoUpload;
    uint32_t lastAudio=started;bool audioOpen=false;
    uint32_t lastObservation = started - kVideoIntervalMs;
    bool ready = false, reconnect = false;
    size_t messageBytes = 0;
    int remaining = 0;
    uint32_t frameEpoch = state.value->epoch;
    char chunk[2048];
    int16_t microphone[640];
    const auto epochEvent = [&](Event event, const char* value) {
      if (state.value->epoch && mediaEpoch(cb) == state.value->epoch) emit(event, value);
    };
    const auto synchronizeEpoch = [&]() -> bool {
      const uint32_t next = mediaEpoch(cb);
      if (!persistent || next == state.value->epoch) return true;
      if (state.value->epoch) {
        const bool oldWork = state.value->awaitingTurn || state.value->modelBusy || state.value->calls.pending();
        if(audioOpen){char end[]={"{\"realtimeInput\":{\"audioStreamEnd\":true}}"};if(!send(socket.ws,end,cb))return false;audioOpen=false;}
        // Resolve outstanding nonblocking calls before interrupting generation.
        // These are cancellation outcomes, never re-executed actions.
        for (size_t i = 0; i < state.value->calls.count; ++i) {
          auto& call = state.value->calls.entries[i];
          if (!call.pending) continue;
          state.value->calls.cancel(call.id, cb);
          if (!resultReply(socket.ws, call.id, call.name, "cancelled_conversation_ended", cb)) return false;
        }
        char interrupt[] = "{\"clientContent\":{\"turnComplete\":false}}";
        if (!send(socket.ws, interrupt, cb)) return false;
        state.value->resumable = false;
        if (oldWork) { state.value->draining = true; state.value->drainingAt = clockMs(); }
      }
      state.value->calls.cancelAll(cb);
      state.value->epoch = next;
      state.value->clearEvidence();
      state.value->inputSent = state.value->epochHadMedia = false;
      state.value->inputReady = next && !state.value->draining;
      state.value->userSpeaking=state.value->preVadSpeech=false; state.value->modelBusy = state.value->awaitingTurn = false;
      if (ready && state.value->inputReady) epochEvent(Event::InputReady, "Ready for video and typed input");
      return true;
    };
    while (!cb.cancelled(cb.context)) {
      if (!synchronizeEpoch()) { emit(Event::Error, "Could not pause Live conversation"); return false; }
      const uint32_t now = clockMs();
      if (state.value->draining && now - state.value->drainingAt >= kPauseDrainMs) {
        emit(Event::Error, "Resetting Live connection after paused response"); return false;
      }
      if (!ready && now - started > 12000) { emit(Event::Error, "Gemini did not confirm Live setup"); return false; }
      if (now - lastReceived > 30000) { emit(Event::Error, "Gemini Live stopped responding"); return false; }
      if(state.value->calls.count>=112 && !state.value->modelBusy && !state.value->awaitingTurn && !state.value->calls.pending()) {
        emit(Event::Error,"Live connection refreshing action history"); return false;
      }
      if (state.value->goAway) {
        if (state.value->canResume() && cb.canResume && cb.canResume(cb.context) && !remaining && !messageBytes) {
          reconnect = true; break;
        }
        if (now - state.value->goAwayAt >= state.value->goAwayWaitMs) {
          emit(Event::Error, "Live session ended before a safe resume point"); return false;
        }
      }
      if (now - lastPing >= 10000) {
        if (!heartbeat(socket.ws)) { emit(Event::Error, "Gemini Live connection lost"); return false; }
        lastPing = now;
      }
      if (ready && (!persistent || (state.value->epoch && state.value->inputReady && !state.value->draining))) {
        const bool steeringTask = cb.taskActive && cb.taskActive(cb.context);
        if ((cb.readText || cb.readRecovery) && !state.value->goAway &&
            (steeringTask || (!state.value->awaitingTurn && !state.value->modelBusy)) &&
            !state.value->calls.pending() &&
            !remaining && !messageBytes && esp_transport_poll_read(socket.ws, 0) == 0) {
          // Fresh steering need not wait for the entire model turn, but must
          // wait for its outstanding tool and preserve pending results.
          // Reuse writable PSRAM; the socket remains owned by this worker.
          char* input = reinterpret_cast<char*>(buffers.scratch);
          const size_t recoveryLength = cb.readRecovery ? cb.readRecovery(input, kMaxTextLength + 1, cb.context) : 0;
          const size_t length = recoveryLength ? recoveryLength : cb.readText ? cb.readText(input, kMaxTextLength + 1, cb.context) : 0;
          if (persistent && mediaEpoch(cb) != state.value->epoch) continue;
          if (length > kMaxTextLength) { emit(Event::Error, "Chat message exceeded its size limit"); return false; }
          if (length) {
            input[length] = 0;
            Json message{cJSON_CreateObject()};
            auto* realtime = message.value ? cJSON_AddObjectToObject(message.value, "realtimeInput") : nullptr;
            if (!realtime || !cJSON_AddStringToObject(realtime, "text", input) ||
                !sendJson(socket.ws, message.value, cb, persistent ? state.value->epoch : 0)) {
              emit(Event::Error, "Could not send chat message; it will not be retried"); return false;
            }
            if (persistent && mediaEpoch(cb) != state.value->epoch) continue;
            state.value->startActivity(); state.value->transcript();
            state.value->inputSent = state.value->epochHadMedia = state.value->awaitingTurn = true;
            state.value->modelBusy = true;
            if (recoveryLength) epochEvent(Event::RecoverySent, input);
            else {
              epochEvent(Event::UserPrompt, input);
              epochEvent(Event::InputText, input);
            }
          }
        }
        // Upload raw microphone PCM directly to ER 2. No local command
        // classifier and no cloud speech playback exist in this session.
        if(cb.readAudio&&!state.value->goAway) for(unsigned i=0;i<4;++i){
          const size_t count=cb.readAudio(microphone,640,cb.context);
          if(persistent&&mediaEpoch(cb)!=state.value->epoch)break;
          if(count>640){emit(Event::Error,"Microphone stream interrupted");return false;}
          if(!count)break;
          WriteFailure audioFailure;
          if(!media(socket.ws,buffers.outgoing,kOutgoingBytes,"audio","audio/pcm;rate=16000",
              reinterpret_cast<const uint8_t*>(microphone),count*2,cb,persistent?state.value->epoch:0,&audioFailure)){
            char reason[128];
            snprintf(reason,sizeof(reason),"Could not send microphone audio: %s %d/%u at byte %u",audioFailure.reason,
                audioFailure.written,unsigned(audioFailure.expected),unsigned(audioFailure.offset));
            emit(Event::Error,reason);return false;
          }
          if(persistent&&mediaEpoch(cb)!=state.value->epoch)break;
          state.value->inputSent=state.value->epochHadMedia=true;lastAudio=clockMs();audioOpen=true;
          if(localSpeechEvidence(microphone,count)){
            if(!state.value->preVadSpeech&&clockMs()-state.value->preVadAt>1500)state.value->clearEvidence();
            state.value->preVadSpeech=true;state.value->preVadAt=clockMs();
          }
          if(count<640)break;
        }
        if(audioOpen&&clockMs()-lastAudio>=1000){char end[]={"{\"realtimeInput\":{\"audioStreamEnd\":true}}"};
          if(!send(socket.ws,end,cb)){emit(Event::Error,"Could not end microphone activity");return false;}audioOpen=false;}
        if(state.value->preVadSpeech&&!state.value->userSpeaking&&!state.value->awaitingTurn&&clockMs()-state.value->preVadAt>1500)state.value->preVadSpeech=false;
        // Every camera upload shares this limiter, including tool observations.
        if (persistent && mediaEpoch(cb) != state.value->epoch) continue;
        if (!flushToolResults(socket.ws, *state.value, cb)) { emit(Event::Error, "Could not report Live action result"); return false; }
        if (buffers.video && !state.value->goAway && !remaining && !messageBytes &&
            clockMs() - lastVideo >= kVideoIntervalMs && esp_transport_poll_read(socket.ws, 0) == 0) {
          lastVideo = clockMs();  // No retries or old-frame queue under congestion.
          uint32_t sequence = 0;
          const size_t bytes = cb.readVideo(buffers.video, kMaxVideo, &sequence, cb.context);
          if (persistent && mediaEpoch(cb) != state.value->epoch) continue;
          if (bytes > kMaxVideo) { emit(Event::Error, "Live camera frame exceeded its size limit"); return false; }
          if (bytes && bytes >= 4 && buffers.video[0] == 0xff && buffers.video[1] == 0xd8 &&
              buffers.video[bytes - 2] == 0xff && buffers.video[bytes - 1] == 0xd9) {
            WriteFailure videoFailure;
            const uint32_t uploadStarted=clockMs();
            if (!media(socket.ws, buffers.outgoing, kOutgoingBytes, "video", "image/jpeg", buffers.video, bytes, cb,
                persistent ? state.value->epoch : 0, &videoFailure)) {
              if (cb.cancelled(cb.context) || (persistent && mediaEpoch(cb)!=state.value->epoch)) return false;
              char reason[128];
              snprintf(reason,sizeof(reason),"Live camera upload failed: %s %d/%u at byte %u",videoFailure.reason,
                  videoFailure.written,unsigned(videoFailure.expected),unsigned(videoFailure.offset));
              emit(Event::Error, reason); return false;
            }
            if (persistent && mediaEpoch(cb) != state.value->epoch) continue;
            state.value->epochHadMedia = true;
            // Start at most one new image per second. Including write time in
            // the period slowed the healthy device to about 0.85 FPS.
            lastVideo = lastVideoUpload = uploadStarted; // Shared across reconnects; never catch up.
            if (cb.videoSent && !cb.cancelled(cb.context) && mediaEpoch(cb) == state.value->epoch) cb.videoSent(buffers.video, bytes, sequence, cb.context);
          }
        }
        if (cb.readHeartbeat && !state.value->userSpeaking && !state.value->preVadSpeech && !state.value->goAway && !state.value->modelBusy && !state.value->awaitingTurn &&
            !state.value->calls.pending() && !remaining && !messageBytes &&
            clockMs()-lastObservation>=kVideoIntervalMs && esp_transport_poll_read(socket.ws,0)==0) {
          char input[kMaxTextLength+1]{};
          const size_t length=cb.readHeartbeat(input,sizeof(input),cb.context);
          if (persistent && mediaEpoch(cb)!=state.value->epoch) continue;
          if(length>kMaxTextLength) { emit(Event::Error,"Observation prompt exceeded its size limit"); return false; }
          if(length) {
            input[length]=0;
            Json message{cJSON_CreateObject()};
            auto* realtime=message.value?cJSON_AddObjectToObject(message.value,"realtimeInput"):nullptr;
            if(!realtime||!cJSON_AddStringToObject(realtime,"text",input)||!sendJson(socket.ws,message.value,cb,state.value->epoch)) {
              emit(Event::Error,"Could not send observation prompt"); return false;
            }
            if (persistent && mediaEpoch(cb)!=state.value->epoch) continue;
            lastObservation=clockMs();
            state.value->inputSent=state.value->awaitingTurn=state.value->modelBusy=true;
            epochEvent(Event::HeartbeatSent,"");
          }
        }
      }
      const int readable = esp_transport_poll_read(socket.ws, 5);
      if (readable < 0) { emit(Event::Error, "Gemini Live connection lost"); return false; }
      if (!readable && !remaining) continue;
      const int count = esp_transport_read(socket.ws, chunk, sizeof(chunk), 250);
      if (cb.cancelled(cb.context)) return false;
      if (count < 0 || (!count && remaining)) { emit(Event::Error, "Gemini Live connection lost during a message"); return false; }
      const auto opcode = esp_transport_ws_get_read_opcode(socket.ws);
      if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
        const unsigned code = count >= 2 ? (unsigned(static_cast<uint8_t>(chunk[0])) << 8) |
            static_cast<uint8_t>(chunk[1]) : 0;
        char reason[128];
        const char* hint = code == 1007 ? "setup rejected; check model and instructions" :
            code == 1008 ? "request refused; check key permissions and quota" :
            code == 1009 ? "message too large" :
            code == 1011 || code == 1013 ? "temporary service failure; retrying automatically" :
            "connection ended; retrying automatically";
        snprintf(reason, sizeof(reason), "Gemini closed Live (%u): %s", code, hint);
        emit(Event::Error, reason); return false;
      }
      if (opcode == WS_TRANSPORT_OPCODES_PING) {
        if (count > 125) { emit(Event::Error, "Invalid Live heartbeat"); return false; }
        // chunk is writable even for the zero-byte control frame.
        if (esp_transport_ws_send_raw(socket.ws, static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN),
            chunk, count, 500) != count) { emit(Event::Error, "Could not answer Live heartbeat"); return false; }
        lastReceived = clockMs(); continue;
      }
      if (opcode == WS_TRANSPORT_OPCODES_PONG) { lastReceived = clockMs(); continue; }
      if (!count) continue;
      lastReceived = clockMs();
      if (!remaining) {
        if ((messageBytes && opcode != WS_TRANSPORT_OPCODES_CONT) ||
            (!messageBytes && opcode != WS_TRANSPORT_OPCODES_TEXT && opcode != WS_TRANSPORT_OPCODES_BINARY)) {
          emit(Event::Error, "Invalid Live message sequence"); return false;
        }
        remaining = esp_transport_ws_get_read_payload_len(socket.ws);
        if (!messageBytes) frameEpoch = state.value->epoch;
        if (remaining <= 0 || static_cast<size_t>(remaining) > kMaxMessage - messageBytes) {
          emit(Event::Error, "Live response is too large"); return false;
        }
      }
      if (count > remaining) { emit(Event::Error, "Invalid Live message length"); return false; }
      memcpy(buffers.message + messageBytes, chunk, count);
      messageBytes += count; remaining -= count;
      if (remaining || !esp_transport_ws_get_fin_flag(socket.ws)) continue;
      buffers.message[messageBytes] = 0;
      if (!boundedJson(buffers.message, messageBytes)) { emit(Event::Error, "Malformed Live response"); return false; }
      Json message{cJSON_ParseWithLengthOpts(buffers.message, messageBytes + 1, nullptr, true)};
      messageBytes = 0;
      if (!uniqueFields(message.value)) { emit(Event::Error, "Invalid Live response"); return false; }
      if (const auto* error = field(message.value, "error")) {
        const auto* code = field(error, "code");
        const int status = cJSON_IsNumber(code) ? code->valueint : 0;
        const char* hint = status == 401 || status == 403 ? "API key refused; check key permissions" :
            status == 429 ? "rate limit or quota reached" :
            status >= 500 ? "service unavailable; retrying automatically" : "setup rejected; check model and settings";
        char reason[128]; snprintf(reason, sizeof(reason), "Gemini Live error (%d): %s", status, hint);
        emit(Event::Error, reason); return false;
      }
      if (field(message.value, "setupComplete")) {
        retryMs = 1000;  // A healthy connection resets prior failure backoff.
        ready = true; emit(Event::Connected, probe ? "Connection verified" : "Connection ready");
        if (probe) return false;
        if (persistent) {
          state.value->epoch = mediaEpoch(cb);
          state.value->inputReady = state.value->epoch != 0;
          if (state.value->inputReady) epochEvent(Event::InputReady, "Ready for video and typed input");
        }
      }
      if (!ready) continue;
      updateResumption(*state.value, field(message.value, "sessionResumptionUpdate"));
      updateGoAway(*state.value, field(message.value, "goAway"));
      if (!synchronizeEpoch()) { emit(Event::Error, "Could not pause Live conversation"); return false; }
      const auto* content = field(message.value, "serverContent");
      if (persistent && state.value->draining) {
        if (const auto* call = field(message.value, "toolCall")) {
          state.value->resumable = false;
          if (!toolReply(socket.ws, call, state.value->calls, cb, state.value, false)) {
            emit(Event::Error, "Could not reject paused Live action"); return false;
          }
        }
        if (cJSON_IsTrue(field(content, "turnComplete")) || cJSON_IsTrue(field(content, "interrupted"))) {
          state.value->draining = false;
          state.value->inputReady = state.value->epoch != 0;
          if (state.value->inputReady) epochEvent(Event::InputReady, "Ready for video and typed input");
        }
        continue;  // The barrier message still belongs to the paused turn.
      }
      const bool activePacket = !persistent || (state.value->epoch && state.value->inputReady &&
          state.value->inputSent && frameEpoch == state.value->epoch && mediaEpoch(cb) == state.value->epoch);
      if (!activePacket) {
        if (const auto* call = field(message.value, "toolCall")) {
          state.value->resumable = false;
          if (!toolReply(socket.ws, call, state.value->calls, cb, state.value, false)) {
            emit(Event::Error, "Could not reject idle Live action"); return false;
          }
        }
        continue;
      }
      const char* activity=text(field(field(message.value,"voiceActivity"),"type"));
      if(!strcmp(activity,"ACTIVITY_START")){
        state.value->startActivity();state.value->userSpeaking=state.value->awaitingTurn=true;
        epochEvent(Event::UserActivity,"");
      }else if(!strcmp(activity,"ACTIVITY_END"))state.value->userSpeaking=false;
      if(const char* input=text(field(field(content,"inputTranscription"),"text"));*input){
        // Accept transcripts only after transmitted local speech evidence or
        // server VAD; camera/heartbeat/model text cannot become a user task.
        if(state.value->activitySeen || (state.value->preVadSpeech&&clockMs()-state.value->preVadAt<=15000)){
          if(!state.value->activitySeen)state.value->startActivity();
          if(state.value->transcript())epochEvent(Event::UserPrompt,input);
          state.value->modelBusy=state.value->awaitingTurn=true;
          epochEvent(Event::InputText,input);
        }
      }
      const bool interrupted = cJSON_IsTrue(field(content, "interrupted"));
      if (interrupted) {
        state.value->modelBusy=state.value->awaitingTurn=state.value->resumable=false;
        epochEvent(Event::Interrupted, "");
      }
      if (const auto* cancellation = field(message.value, "toolCallCancellation")) {
        const auto* ids = field(cancellation, "ids");
        if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > 128) { emit(Event::Error, "Invalid Live action cancellation"); return false; }
        const cJSON* id = nullptr;
        cJSON_ArrayForEach(id, ids) {
          if (!cJSON_IsString(id) || strlen(text(id)) > 128) { emit(Event::Error, "Invalid Live action cancellation"); return false; }
          state.value->calls.cancel(text(id), cb);
        }
        state.value->resumable = false;
      }
      const bool outputAllowed=!persistent||state.value->outputAllowed();
      const auto* parts=field(field(content,"modelTurn"),"parts");
      const cJSON* part=nullptr;
      cJSON_ArrayForEach(part,parts) {
        if(interrupted||!outputAllowed||mediaEpoch(cb)!=state.value->epoch) continue;
        // Thinking summaries are not user-facing replies. ER 2 has no audio
        // playback path; inline data is never scratch or sent to the speaker.
        if(cJSON_IsTrue(field(part,"thought"))) continue;
        const char* output=text(field(part,"text"));
        if(*output) { state.value->modelBusy=true; epochEvent(Event::OutputText,output); }
      }
      if(cJSON_IsTrue(field(content,"generationComplete"))&&outputAllowed) epochEvent(Event::GenerationComplete,"");
      if(cJSON_IsTrue(field(content,"turnComplete"))) {
        state.value->modelBusy=state.value->awaitingTurn=state.value->userSpeaking=state.value->preVadSpeech=false;
        state.value->clearEvidence();
        if(outputAllowed) epochEvent(Event::TurnComplete,"");
      }
      if (const auto* call = field(message.value, "toolCall"); call && !interrupted) {
        state.value->resumable = false;
        if (!toolReply(socket.ws, call, state.value->calls, cb, state.value)) { emit(Event::Error, "Could not process Live action request"); return false; }
      }
    }
    return reconnect;
    };
    const bool safeResume = connection();  // Destroy the old TLS socket first.
    if (cb.cancelled(cb.context) || probe || (!persistent && !safeResume)) return;
    if (safeResume) { retryMs = 1000; continue; }
    // Failed connections get a fresh session, never replay media, tools or
    // responses. The lifetime dedup table remains intact across reconnects.
    state.value->calls.cancelAll(cb);
    state.value->resume[0] = 0;
    state.value->calls = {};
    state.value->resumable = state.value->goAway = state.value->userSpeaking=state.value->preVadSpeech=false; state.value->modelBusy = state.value->awaitingTurn = false;
    state.value->draining = state.value->inputReady = state.value->inputSent = state.value->epochHadMedia = false;
    state.value->clearEvidence();
    const uint32_t retryAt = clockMs();
    char retryNotice[48]; snprintf(retryNotice, sizeof(retryNotice), "Retrying Gemini in %lu s", static_cast<unsigned long>(retryMs / 1000));
    emit(Event::Retrying, retryNotice);
    while (!cb.cancelled(cb.context) && clockMs() - retryAt < retryMs) vTaskDelay(pdMS_TO_TICKS(50));
    retryMs = std::min<uint32_t>(30000, retryMs * 2);
  }
}
}  // namespace gemini_live
