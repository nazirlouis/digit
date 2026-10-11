#include "speech_commands.h"
#include "speech_pipeline.h"
#include "gemini_client.h"
#include "gemini_live.h"
#include "live_audio.h"
#include "robot_actions.h"
#include "zumo_link.h"
#include "reaction_player.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "wake_word.h"
#include "Arduino.h"
#include "task_policy.h"
#include <cstring>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace {
constexpr size_t kOutputSamples = 16000 * 8;
constexpr uint32_t kMicFreshMs = 500, kToolFreshMs = 1000, kActivationWaitMs = 20000;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
speech_commands::Status state{};
QueueHandle_t inputQueue = nullptr;
SemaphoreHandle_t outputLock = nullptr;
int16_t* outputPcm = nullptr;
size_t outputHead = 0, outputCount = 0;
uint32_t outputSession = 0;
uint32_t playbackEpoch = 1, outputEpoch = 0;
live_audio::Resampler resampler;
uint32_t session = 1, generation = 0, opensAt = 0, safetyEvents = 0;
uint32_t idleMs = 30000, idleAt = 0, offeredVideoAt = 0;
uint32_t responseProgressAt = 0, activationAt = 0;
uint32_t connection = 1, connectionRevision = 0;
bool wasConnected = false, waitingForVoice = false, workerBusy = false, inputReady = false;
bool newInput = true, newOutput = true, responsePending = false, idleArmed = false;
bool awaitingReadyReaction = false;
int pendingCommand = 0;
robot_actions::ToolRequest pendingTool{};
robot_actions::TaskPolicy task;
bool observationPending=false, observationSent=false, expressionPending=false, stopExpression=false;
uint32_t observationAfter=0, observationRequestedAt=0, expressionAt=0;
int completedTravel=0, completedTurn=0;
char observationCall[129]{}, expressionCall[129]{}, observationOutcome[32]{};
uint32_t pendingSession = 0, pendingGeneration = 0, pendingAt = 0;
char pendingCall[129]{};
bool ownedAction = false, stopOwned = false, toolResultReady = false, toolStopPending = false;
bool dispatching = false, dispatchCancelled = false;
uint32_t ownedGeneration = 0, ownedSession = 0;
char ownedCall[129]{}, toolResultCall[129]{}, toolResultText[768]{};
struct Request { uint32_t connection, revision, started, mediaSession; };

bool currentConnection(const Request& request) {
  const uint32_t revision = gemini_client::configurationRevision();
  portENTER_CRITICAL(&lock);
  const bool active = request.connection == connection && request.revision == revision && state.ready;
  portEXIT_CRITICAL(&lock);
  return active;
}
bool current(const Request& request) {
  if (!currentConnection(request)) return false;
  portENTER_CRITICAL(&lock);
  const bool active = request.connection == connection && request.mediaSession == session && state.liveActive;
  portEXIT_CRITICAL(&lock);
  return active;
}
bool cancelled(void* context) { return !currentConnection(*static_cast<Request*>(context)); }
uint32_t mediaEpoch(void* context) {
  auto& request = *static_cast<Request*>(context);
  portENTER_CRITICAL(&lock);
  request.mediaSession = request.connection == connection && state.liveActive ? session : 0;
  const uint32_t epoch = request.mediaSession;
  portEXIT_CRITICAL(&lock);
  return epoch;
}
void clearPlayback() {
  portENTER_CRITICAL(&lock); ++playbackEpoch; portEXIT_CRITICAL(&lock);
  // Session tags prevent stale playback even if the producer owns the ring.
  if (outputLock && xSemaphoreTake(outputLock, 0) == pdTRUE) {
    outputCount = outputHead = outputSession = 0; resampler = {};
    xSemaphoreGive(outputLock);
  }
}
void endSession(const char* result) {
  portENTER_CRITICAL(&lock);
  if (ownedAction) stopOwned = true;
  if (expressionPending) stopExpression = true;
  ++session; waitingForVoice = awaitingReadyReaction = inputReady = false; pendingCommand = 0;
  pendingCall[0] = 0; toolResultReady = responsePending = idleArmed = false;
  state.liveActive = state.liveSpeaking = state.liveProbe = false;
  state.listening = state.thinking = state.pendingAction = false;
  state.idleRemainingMs = 0;
  if (task.active) snprintf(state.taskResult,sizeof(state.taskResult),"Cancelled: %.100s",result);
  task.active=state.taskActive=false; observationPending=observationSent=expressionPending=false;
  snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",result);
  snprintf(state.result, sizeof(state.result), "%s", result);
  portEXIT_CRITICAL(&lock);
  if (inputQueue) xQueueReset(inputQueue);
  clearPlayback();
}
void appendText(char* destination, size_t size, const char* value, bool& fresh) {
  if (fresh) { destination[0] = 0; fresh = false; }
  const size_t used = strlen(destination);
  if (used < size) snprintf(destination + used, size - used, "%s", value);
}
void completeTool(const char* callId, const char* result);
void event(gemini_live::Event kind, const char* value, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!currentConnection(request)) return;
  // Connection events remain valid while the media epoch is private/idle.
  if (kind == gemini_live::Event::Connecting) {
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    const bool end = state.liveConnected && state.liveActive;
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = true;
    snprintf(state.result, sizeof(state.result), "Connecting");
    portEXIT_CRITICAL(&lock);
    if (end) endSession("Connection refreshing; say Digit again");
    if (inputQueue) xQueueReset(inputQueue);
    return;
  }
  if (kind == gemini_live::Event::Connected) {
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    if (!state.liveConnected) ++state.liveConnectionCount;
    state.liveConnected = true; state.liveConnecting = false;
    state.latencyMs = millis() - request.started; state.error[0] = 0;
    const bool probe = state.liveProbe; state.liveProbe = false;
    snprintf(state.result, sizeof(state.result), "%s", probe ? "Connection verified" : state.liveActive ? "Preparing conversation" : "Ready; say Digit");
    portEXIT_CRITICAL(&lock);
    return;
  }
  if (kind == gemini_live::Event::Error) {
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = true;
    snprintf(state.error, sizeof(state.error), "%s", value);
    portEXIT_CRITICAL(&lock);
    endSession("Connection lost; say Digit when ready");
    return;
  }
  if (!current(request)) return;
  if (kind == gemini_live::Event::Interrupted || kind == gemini_live::Event::Error) clearPlayback();
  portENTER_CRITICAL(&lock);
  if (session == request.mediaSession && state.liveActive) {
    switch (kind) {
      case gemini_live::Event::Connecting:
      case gemini_live::Event::Connected:
      case gemini_live::Event::Error:
        break;  // handled above regardless of the media epoch.
      case gemini_live::Event::InputReady:
        inputReady = true; state.thinking = false;
        state.listening = !waitingForVoice;
        idleAt = millis(); idleArmed = !waitingForVoice;
        snprintf(state.result, sizeof(state.result), "%s", waitingForVoice ? "Wake acknowledgment" : "Live listening"); break;
      case gemini_live::Event::UserActivity:
        idleAt = responseProgressAt = millis(); idleArmed = false; responsePending = state.thinking = true; break;
      case gemini_live::Event::InputText:
        idleAt = responseProgressAt = millis(); idleArmed = false; responsePending = state.thinking = true;
        appendText(state.transcript, sizeof(state.transcript), value, newInput);
        snprintf(state.decoderText, sizeof(state.decoderText), "%.127s", state.transcript); break;
      case gemini_live::Event::OutputText:
        responseProgressAt = millis(); responsePending = true; idleArmed = false; state.thinking = false;
        appendText(state.liveResponse, sizeof(state.liveResponse), value, newOutput); break;
      case gemini_live::Event::Interrupted:
        state.liveSpeaking = state.thinking = false; newOutput = true;
        if (pendingCommand) {
          completeTool(pendingCall, "Rejected: user interrupted before action dispatch");
          pendingCommand = 0; pendingCall[0] = 0; state.pendingAction = false;
        }
        if (dispatching) dispatchCancelled = true;
        // Cancellation of a running tool is explicit and carries its call ID.
        snprintf(state.result, sizeof(state.result), "Live listening"); break;
      case gemini_live::Event::GenerationComplete:
        break;  // turnComplete and speaker drain define the idle boundary.
      case gemini_live::Event::TurnComplete:
        newInput = newOutput = true; responsePending = state.thinking = false; break;
    }
  }
  portEXIT_CRITICAL(&lock);
}
size_t readAudio(int16_t* samples, size_t capacity, void* context) {
  auto& request = *static_cast<Request*>(context);
  if (!current(request)) return 0;
  portENTER_CRITICAL(&lock); const bool open = inputReady && state.listening; portEXIT_CRITICAL(&lock);
  if (!open) return 0;
  size_t count = 0;
  speech_pipeline::Frame frame{};
  while (count + speech_pipeline::kInputSamples <= capacity && xQueueReceive(inputQueue, &frame, 0) == pdTRUE) {
    if (frame.session != request.mediaSession) continue;
    if (millis() - frame.capturedAt > kMicFreshMs) {
      event(gemini_live::Event::Error, "Live microphone fell behind; say Digit to try again", context);
      return capacity + 1;
    }
    memcpy(samples + count, frame.samples, frame.count * sizeof(int16_t)); count += frame.count;
    portENTER_CRITICAL(&lock);
    if (session == request.mediaSession && state.liveActive) {
      ++state.fetchChunks; ++state.totalFetches; state.liveSentSamples += frame.count;
    }
    portEXIT_CRITICAL(&lock);
  }
  return count;
}
bool playAudio(const int16_t* samples, size_t count, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || xSemaphoreTake(outputLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  portENTER_CRITICAL(&lock); const uint32_t expectedPlayback = playbackEpoch; portEXIT_CRITICAL(&lock);
  bool ok = current(request);
  if (ok && (outputSession != request.mediaSession || outputEpoch != expectedPlayback)) {
    outputSession = request.mediaSession; outputEpoch = expectedPlayback; outputHead = outputCount = 0; resampler = {};
  }
  if (ok) ok = resampler.append(samples, count, [](const int16_t* pcm, size_t amount) {
    if (amount > kOutputSamples - outputCount) return false;
    for (size_t i = 0; i < amount; ++i) outputPcm[(outputHead + outputCount++) % kOutputSamples] = pcm[i];
    return true;
  });
  xSemaphoreGive(outputLock);
  portENTER_CRITICAL(&lock);
  if (ok && session == request.mediaSession && state.liveActive && playbackEpoch == expectedPlayback) {
    state.liveReceivedSamples += static_cast<uint32_t>(count); state.liveSpeaking = true; idleArmed = false; state.thinking = false;
    responseProgressAt = millis();
    snprintf(state.result, sizeof(state.result), "Live speaking");
  }
  portEXIT_CRITICAL(&lock);
  return ok;
}
size_t readVideo(uint8_t* destination, size_t capacity, uint32_t* sequence, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !destination || !sequence) return 0;
  portENTER_CRITICAL(&lock);
  const bool open = inputReady && state.listening && !waitingForVoice && observationPending && !observationSent;
  const uint32_t after = observationAfter;
  portEXIT_CRITICAL(&lock);
  if (!open) return 0;
  camera_service::Frame frame{};
  if (!camera_service::copyLatestJpeg(destination, capacity, &frame) || millis() - frame.timestampMs > 500) return 0;
  portENTER_CRITICAL(&lock);
  const bool fresh = session == request.mediaSession && state.liveActive && frame.timestampMs != offeredVideoAt && static_cast<int32_t>(frame.timestampMs-after)>0;
  if (fresh) { offeredVideoAt = frame.timestampMs; *sequence = frame.timestampMs; }
  portEXIT_CRITICAL(&lock);
  return fresh ? frame.size : 0;
}
void videoSent(size_t bytes, uint32_t sequence, void* context) {
  const auto& request = *static_cast<Request*>(context);
  portENTER_CRITICAL(&lock);
  if (request.mediaSession == session && state.liveActive) {
    ++state.liveVideoFrames; state.liveVideoBytes += static_cast<uint32_t>(bytes); state.liveVideoCapturedAt = sequence;
    if (observationPending && static_cast<int32_t>(sequence-observationAfter)>0) observationSent=true;
  }
  portEXIT_CRITICAL(&lock);
}
gemini_live::ActionResult action(const char* callId, const robot_actions::ToolRequest& tool, void* context) {
  const int command=static_cast<int>(tool.command);
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !callId || !*callId || strlen(callId) >= sizeof(pendingCall) || !robot_actions::validCommand(command))
    return gemini_live::ActionResult::Rejected;
  // Stop preempts queued results, observations and in-flight dispatch. Only
  // the control loop touches hardware; cancelling the epoch is immediate.
  if (tool.command==robot_actions::Command::Stop) {
    portENTER_CRITICAL(&lock);
    const bool active=request.mediaSession==session && state.liveActive;
    if (active) toolStopPending=true;
    portEXIT_CRITICAL(&lock);
    if (active) endSession("Gemini Stop");
    return active ? gemini_live::ActionResult::Queued : gemini_live::ActionResult::Rejected;
  }
  portENTER_CRITICAL(&lock);
  gemini_live::ActionResult result = gemini_live::ActionResult::Rejected;
  if (request.mediaSession == session && state.liveActive && inputReady) {
    state.candidateCommand = command; ++state.recognized;
    if (!dispatching && !pendingCommand && !toolResultReady &&
        ((!ownedAction && !observationPending && !expressionPending) || tool.command==robot_actions::Command::Stop ||
          (tool.command==robot_actions::Command::Expression && !expressionPending))) {
      pendingTool=tool; pendingCommand=command; pendingSession=session; pendingGeneration=generation; pendingAt=millis();
      snprintf(pendingCall,sizeof(pendingCall),"%s",callId);
      state.pendingAction=true; idleArmed=false;
      snprintf(state.reason,sizeof(state.reason),"Tool request awaiting local checks");
      result=gemini_live::ActionResult::Queued;
    } else snprintf(state.reason,sizeof(state.reason),"Wait for the current tool result");
  }
  portEXIT_CRITICAL(&lock);
  return result;
}
void cancelTool(const char* callId, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request)) return;
  bool end = false;
  portENTER_CRITICAL(&lock);
  if (request.mediaSession == session && state.liveActive && callId) {
    if (!strcmp(pendingCall, callId)) {
      if (dispatching) dispatchCancelled = true;
      else { pendingCommand = 0; pendingCall[0] = 0; state.pendingAction = false; }
    }
    if (toolResultReady && !strcmp(toolResultCall, callId)) toolResultReady = false;
    if ((ownedAction && !strcmp(ownedCall, callId)) || (observationPending && !strcmp(observationCall,callId)) || (expressionPending && !strcmp(expressionCall,callId))) end=true;
  }
  portEXIT_CRITICAL(&lock);
  if (end) endSession("Action cancelled by Gemini");
}
bool pollToolResult(char* callId, size_t idCapacity, char* result, size_t resultCapacity, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !callId || idCapacity < sizeof(toolResultCall) || !result || resultCapacity < sizeof(toolResultText)) return false;
  portENTER_CRITICAL(&lock);
  const bool ready = request.mediaSession == session && state.liveActive && toolResultReady;
  if (ready) {
    snprintf(callId, idCapacity, "%s", toolResultCall); snprintf(result, resultCapacity, "%s", toolResultText);
    toolResultReady = false;
  }
  portEXIT_CRITICAL(&lock);
  return ready;
}
bool canResume(void* context) {
  const auto& request = *static_cast<Request*>(context);
  portENTER_CRITICAL(&lock);
  const bool safe = request.connection == connection && !state.liveSpeaking &&
      !responsePending && !pendingCommand && !ownedAction && !toolResultReady && !dispatching && !task.active && !observationPending && !expressionPending;
  portEXIT_CRITICAL(&lock);
  return safe;
}
bool taskActive(void* context) {
  const auto& request=*static_cast<Request*>(context);
  if (!current(request)) return false;
  portENTER_CRITICAL(&lock); const bool active=task.active; portEXIT_CRITICAL(&lock); return active;
}
void worker(void*) {
  uint32_t retryMs = 1000;
  for (;;) {
    const auto config = gemini_client::settings();
    const uint32_t revision = gemini_client::configurationRevision();
    portENTER_CRITICAL(&lock);
    const bool start = state.ready && config.configured;
    Request request{++connection, revision, millis(), 0};
    if (start) { workerBusy = true; connectionRevision = revision; state.liveConnecting = true; }
    portEXIT_CRITICAL(&lock);
    if (!start) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    const gemini_live::Callbacks callbacks{&request, cancelled, readAudio, playAudio, event, action,
                                          readVideo, videoSent, cancelTool, pollToolResult, canResume, mediaEpoch, taskActive};
    // Persistent worker: idle does not read mic/video, produce audio or tools.
    gemini_client::live(callbacks, false);
    endSession("Connection unavailable; say Digit when ready");
    portENTER_CRITICAL(&lock);
    workerBusy = false; state.liveConnected = state.liveConnecting = false;
    const bool connectedOnce = state.liveConnectionCount != 0;
    portEXIT_CRITICAL(&lock);
    const uint32_t replacement = gemini_client::configurationRevision();
    if (replacement != revision) retryMs = 1000;
    else {
      // Client can return before transport entry while Wi-Fi/time is unavailable.
      vTaskDelay(pdMS_TO_TICKS(retryMs));
      retryMs = connectedOnce ? 1000 : min(retryMs * 2, uint32_t(30000));
    }
  }
}
bool open(uint32_t control, bool probe, bool waitForVoice, bool testOnly) {
  const auto config = gemini_client::settings();
  const auto zumo = zumo_link::snapshot();
  bool accepted = false;
  portENTER_CRITICAL(&lock);
  if (!config.configured) {
    snprintf(state.error, sizeof(state.error), "Add a Gemini API key in the dashboard");
    snprintf(state.result, sizeof(state.result), "Not configured");
  } else if (state.liveActive || ownedAction || stopOwned || dispatching || toolStopPending) {
    snprintf(state.reason, sizeof(state.reason), "Wait for the current conversation to finish");
  } else if (state.ready) {
    if (probe) {
      state.liveProbe = !state.liveConnected;
      state.error[0] = state.reason[0] = 0;
      snprintf(state.result, sizeof(state.result), "%s", state.liveConnected ? "Connection verified" : "Checking connection");
      portEXIT_CRITICAL(&lock);
      return true;
    }
    ++session; generation = control; pendingCommand = 0; toolResultReady = false;
    task={}; observationPending=observationSent=expressionPending=false;
    state.taskActive=false; state.taskMoves=0; state.taskGoal[0]=state.taskResult[0]=0;
    snprintf(state.taskPhase,sizeof(state.taskPhase),"Listening");
    state.liveActive = state.thinking = true; state.liveProbe = false; inputReady = false;
    state.liveSpeaking = state.listening = state.pendingAction = false;
    state.dryRun = testOnly || config.dryRun;
    idleMs = (config.idleTimeoutSec ? config.idleTimeoutSec : 30) * 1000;
    idleAt = responseProgressAt = millis(); idleArmed = responsePending = false; offeredVideoAt = 0; state.idleRemainingMs = idleMs;
    state.error[0] = state.reason[0] = state.transcript[0] = state.liveResponse[0] = state.decoderText[0] = 0;
    state.samples = state.chunks = state.feedChunks = state.fetchChunks = state.liveSentSamples = state.liveReceivedSamples = state.livePlayedSamples = 0;
    state.liveVideoFrames = state.liveVideoBytes = state.liveVideoCapturedAt = 0;
    state.micPeak = 0; state.latencyMs = 0; state.candidateCommand = 0; newInput = newOutput = true;
    safetyEvents = zumo.safetyEvents; wasConnected = zumo.connected;
    opensAt = activationAt = millis(); waitingForVoice = awaitingReadyReaction = waitForVoice;
    snprintf(state.result, sizeof(state.result), "%s", state.liveConnected ? "Preparing conversation" : "Connecting"); accepted = true;
  }
  portEXIT_CRITICAL(&lock);
  return accepted;
}
void completeTool(const char* callId, const char* result) {
  snprintf(toolResultCall, sizeof(toolResultCall), "%s", callId);
  snprintf(toolResultText, sizeof(toolResultText), "%s", result); toolResultReady = true;
}
void requestObservation(const char* callId, const char* outcome, int travel=0, int turn=0) {
  observationPending=true; observationSent=false; observationAfter=observationRequestedAt=millis();
  completedTravel=travel; completedTurn=turn;
  snprintf(observationCall,sizeof(observationCall),"%s",callId);
  snprintf(observationOutcome,sizeof(observationOutcome),"%s",outcome);
  snprintf(state.taskPhase,sizeof(state.taskPhase),"Observing");
  snprintf(state.reason,sizeof(state.reason),"Preparing a fresh camera frame");
}
void failTask(const char* reason) {
  task.active=state.taskActive=false;
  snprintf(state.taskResult,sizeof(state.taskResult),"Failed: %.110s",reason);
  snprintf(state.taskPhase,sizeof(state.taskPhase),"Failed");
}
}  // namespace

namespace speech_commands {
bool liveActive() {
  portENTER_CRITICAL(&lock); const bool active = state.liveActive; portEXIT_CRITICAL(&lock);
  return active;
}
bool liveSpeaking() {
  portENTER_CRITICAL(&lock); const bool speaking = state.liveSpeaking; portEXIT_CRITICAL(&lock);
  return speaking;
}
Activity activity() {
  Activity copy{};
  portENTER_CRITICAL(&lock);
  copy.ready = state.ready; copy.listening = state.listening; copy.pendingAction = state.pendingAction;
  copy.liveActive = state.liveActive; copy.liveConnected = state.liveConnected; copy.liveSpeaking = state.liveSpeaking;
  copy.liveProbe = state.liveProbe; copy.dryRun = state.dryRun;
  memcpy(copy.result, state.result, sizeof(copy.result));
  portEXIT_CRITICAL(&lock);
  const auto settings = gemini_client::settings(); copy.ready = copy.ready && settings.configured;
  if (!copy.liveActive) copy.dryRun = settings.dryRun;
  return copy;
}
void begin() {
  gemini_client::begin(); outputLock = xSemaphoreCreateMutex();
  outputPcm = static_cast<int16_t*>(heap_caps_malloc(kOutputSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticQueue_t queueControl;
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(50 * sizeof(speech_pipeline::Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (storage) inputQueue = xQueueCreateStatic(50, sizeof(speech_pipeline::Frame), storage, &queueControl);
  // TLS/NVS/flash operations require an internal task stack.
  if (!outputLock || !outputPcm || !inputQueue || xTaskCreatePinnedToCore(worker, "gemini-live", 16384, nullptr, 2, nullptr, 0) != pdPASS) {
    snprintf(state.error, sizeof(state.error), "Cannot allocate Gemini Live audio worker"); return;
  }
  state.ready = true; state.dryRun = true; snprintf(state.result, sizeof(state.result), "Connecting when Wi-Fi is ready");
}
void listen(uint32_t control) { open(control, false, true, false); }
bool startLive(uint32_t control, bool probe, bool testOnly) { return open(control, probe, false, testOnly); }
void cancel() { endSession("Conversation ended; say Digit"); }
void submit(const int32_t* stereo, size_t count) {
  if (!inputQueue || !stereo || !count || count > speech_pipeline::kInputSamples) return;
  speech_pipeline::Frame frame{};
  portENTER_CRITICAL(&lock);
  const bool active = state.liveActive && state.liveConnected && inputReady && !waitingForVoice && static_cast<int32_t>(millis() - opensAt) >= 0;
  frame.session = session;
  portEXIT_CRITICAL(&lock);
  if (!active) return;
  frame.capturedAt = millis(); frame.count = static_cast<uint16_t>(count); uint16_t peak = 0;
  for (size_t i = 0; i < count; ++i) {
    const int value = speech_pipeline::mono(stereo[2 * i + 1]); frame.samples[i] = static_cast<int16_t>(value);
    peak = max(peak, static_cast<uint16_t>(value < 0 ? -value : value));
  }
  const bool sent = xQueueSend(inputQueue, &frame, 0) == pdTRUE;
  bool failed = false;
  portENTER_CRITICAL(&lock);
  if (frame.session == session && state.liveActive) {
    if (sent) { state.samples += static_cast<uint32_t>(count); ++state.chunks; ++state.feedChunks; ++state.totalFeeds; state.micPeak = max(state.micPeak, peak); }
    else { ++state.droppedFrames; snprintf(state.error, sizeof(state.error), "Live audio queue overflow; say Digit to try again"); failed = true; }
  }
  portEXIT_CRITICAL(&lock);
  if (failed) endSession("Live error");
}
size_t renderAudio(int16_t* samples, size_t count) {
  if (!samples || !count || !outputLock) return 0;
  portENTER_CRITICAL(&lock);
  const bool active = state.liveActive && state.liveConnected && !state.liveProbe; const uint32_t expected = session, expectedPlayback = playbackEpoch;
  portEXIT_CRITICAL(&lock);
  if (!active || xSemaphoreTake(outputLock, 0) != pdTRUE) return 0;
  size_t read = 0;
  if (outputSession == expected && outputEpoch == expectedPlayback) {
    read = count < outputCount ? count : outputCount;
    for (size_t i = 0; i < read; ++i) samples[i] = outputPcm[(outputHead + i) % kOutputSamples];
    outputHead = (outputHead + read) % kOutputSamples; outputCount -= read;
  }
  const bool speaking = outputEpoch == expectedPlayback && (outputCount || read); xSemaphoreGive(outputLock);
  portENTER_CRITICAL(&lock);
  if (session == expected && state.liveActive && playbackEpoch == expectedPlayback) {
    state.livePlayedSamples += static_cast<uint32_t>(read); state.liveSpeaking = speaking;
    if (state.liveConnected) snprintf(state.result, sizeof(state.result), "%s", speaking ? "Live speaking" : waitingForVoice ? "Wake acknowledgment" : "Live listening");
  } else read = 0;
  portEXIT_CRITICAL(&lock);
  return read;
}
void audioFault() {
  endSession("Microphone unavailable"); portENTER_CRITICAL(&lock);
  state.ready = false; snprintf(state.error, sizeof(state.error), "Microphone unavailable; check audio status");
  state.liveConnected = state.liveConnecting = false;
  portEXIT_CRITICAL(&lock);
}
void networkDisconnected() {
  portENTER_CRITICAL(&lock);
  ++connection; state.liveConnected = state.liveConnecting = state.listening = inputReady = false;
  snprintf(state.error, sizeof(state.error), "Wi-Fi disconnected; waiting to reconnect");
  portEXIT_CRITICAL(&lock);
  endSession("Wi-Fi disconnected; say Digit when ready");
}
void poll() {
  robot_actions::Guard control;
  portENTER_CRITICAL(&lock);
  const bool requestedStop=toolStopPending; toolStopPending=false;
  portEXIT_CRITICAL(&lock);
  if (requestedStop) { robot_actions::stop("Gemini Stop"); return; }
  const uint32_t revision = gemini_client::configurationRevision();
  const auto settings = gemini_client::settings();
  const auto zumo = zumo_link::snapshot(); const auto actions = robot_actions::snapshot();
  bool acknowledge = false;
  portENTER_CRITICAL(&lock);
  const bool configurationChanged = workerBusy && revision != connectionRevision;
  if (configurationChanged) {
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = settings.configured;
  }
  if (!configurationChanged && state.liveActive && state.liveConnected && inputReady && awaitingReadyReaction &&
      generation == actions.generation && (!wasConnected || zumo.connected) && zumo.safetyEvents == safetyEvents) {
    awaitingReadyReaction = false; opensAt = millis(); acknowledge = true;
  }
  portEXIT_CRITICAL(&lock);
  if (configurationChanged) endSession("Connection settings changed; say Digit when ready");
  if (acknowledge) wake_word::conversationReadyReaction();
  const auto audio = audioEngine.snapshot();
  const bool reactionBusy = reaction_player::active();
  bool cancelVoice = false, terminate = false, stopAction = false, cancelFace = false;
  // Guard serializes poll/control callers. Keep these large work buffers off
  // the small ESP-IDF main-task stack, including nested movement dispatch.
  static robot_actions::ToolRequest dispatchTool;
  dispatchTool={};
  int command = 0; uint32_t controlGeneration = 0, expectedSession = 0; char callId[129]{};
  portENTER_CRITICAL(&lock);
  if (stopExpression) { cancelFace=true; stopExpression=false; }
  if (stopOwned) { stopAction = ownedAction && actions.active && actions.generation == ownedGeneration; stopOwned = ownedAction = false; }
  if (state.liveActive && !state.liveProbe && (generation != actions.generation || (wasConnected && !zumo.connected) || zumo.safetyEvents != safetyEvents)) {
    snprintf(state.error, sizeof(state.error), "Live conversation ended because robot control or safety changed"); terminate = true;
  }
  if (state.liveActive && !inputReady && millis() - activationAt >= kActivationWaitMs) {
    snprintf(state.error, sizeof(state.error), "Conversation could not become ready; say Digit when connected"); terminate = true;
  }
  if (!terminate && state.liveActive && state.liveConnected && inputReady && waitingForVoice && !awaitingReadyReaction &&
      ((!reactionBusy && !audio.cuePlaying && !audio.wakeChimePlaying && audio.state != AudioState::Playing) || millis() - opensAt >= 2500)) {
    cancelVoice = reactionBusy || audio.cuePlaying || audio.wakeChimePlaying || audio.state == AudioState::Playing;
    waitingForVoice = false; opensAt = millis() + 120; idleArmed = false;
  }
  state.listening = state.liveActive && state.liveConnected && inputReady && !waitingForVoice && static_cast<int32_t>(millis() - opensAt) >= 0;
  if (!terminate && task.expired(millis())) { failTask("Task time limit reached"); terminate=true; }
  if (!terminate && ownedAction && !actions.active && ownedSession == session && state.liveActive && !toolResultReady && !pendingCommand) {
    ownedAction=false; idleArmed=false; responseProgressAt=millis();
    if (actions.result==robot_actions::Result::Completed) {
      const bool progress=abs(actions.travelledMm)>=3 || abs(actions.turnedDeciDeg)>=10;
      task.moved(progress); state.taskMoves=task.moves;
      requestObservation(ownedCall,"completed",actions.travelledMm,actions.turnedDeciDeg);
      if (task.noProgress>=task.maxNoProgress) failTask("Repeated lack of progress");
    } else {
      completeTool(ownedCall,"Movement failed or cancelled; no further movement authorized");
      failTask(actions.phase);
    }
  }
  if (!terminate && expressionPending && !toolResultReady && !pendingCommand) {
    if (!strcmp(zumo.sceneResult,"APPLIED") || millis()-expressionAt>=3000) {
      completeTool(expressionCall,!strcmp(zumo.sceneResult,"APPLIED") ? "Expression visible; no sound played" : "Expression failed or visibility timed out");
      expressionPending=false; responseProgressAt=millis();
    }
  }
  if (!terminate && observationPending && !toolResultReady && !pendingCommand && (observationSent || millis()-observationRequestedAt>=3500)) {
    static char result[768];
    const bool fresh=zumo.connected && zumo.replyAgeMs<600;
    const bool valid=fresh && zumo.rangeValid && zumo.rangeAgeMs<250;
    const bool noReturn=fresh && zumo.rangeNoReturn && zumo.rangeAgeMs<250;
    if (!observationSent && task.active) failTask("Fresh camera unavailable");
    snprintf(result,sizeof(result),"{\"result\":\"%s\",\"camera_sent\":%s,\"frame_sequence\":%lu,\"travelled_mm\":%d,\"turned_degrees\":%.1f,\"front_mm\":%d,\"front_valid\":%s,\"front_no_return\":%s,\"front_age_ms\":%lu,\"ready\":%s,\"edge\":%s,\"distance_calibrated\":%s,\"task_active\":%s,\"moves_remaining\":%u}",
      observationSent ? observationOutcome : "camera_unavailable",observationSent?"true":"false",static_cast<unsigned long>(observationSent?state.liveVideoCapturedAt:0),completedTravel,completedTurn/10.0,valid?zumo.distanceMm:-1,valid?"true":"false",noReturn?"true":"false",static_cast<unsigned long>(zumo.rangeAgeMs),fresh&&zumo.ready?"true":"false",zumo.edge?"true":"false",zumo.distanceCalibrated?"true":"false",task.active?"true":"false",task.maxMoves-task.moves);
    completeTool(observationCall,result); observationPending=false; responseProgressAt=millis();
    snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",task.active?"Planning next step":"Listening");
    snprintf(state.reason,sizeof(state.reason),"%s",task.active?"Planning the next step":state.taskResult[0]?state.taskResult:"Observation delivered");
  }
  if (!terminate && state.liveActive && pendingCommand) {
    if (pendingSession == session && pendingGeneration == actions.generation && millis() - pendingAt <= kToolFreshMs) {
      dispatchTool=pendingTool; command = pendingCommand; controlGeneration = pendingGeneration; expectedSession = session; snprintf(callId, sizeof(callId), "%s", pendingCall);
      dispatching = true; dispatchCancelled = false;
    } else completeTool(pendingCall, "Rejected: action request expired or robot control changed");
    pendingCommand = 0; state.pendingAction = dispatching;
  }
  if (!terminate && state.liveActive && state.liveConnected && inputReady) {
    // Proactive audio can elect to stay silent. Pongs/video/silence cannot
    // hold a response wait open forever when no model turnComplete arrives.
    if (responsePending && !state.liveSpeaking && !ownedAction && !command && !pendingCommand && !toolResultReady &&
        millis() - responseProgressAt >= idleMs) {
      responsePending = state.thinking = false; idleArmed = true; idleAt = responseProgressAt;
    }
    const bool busy = waitingForVoice || static_cast<int32_t>(millis() - opensAt) < 0 || responsePending || state.liveSpeaking || ownedAction || command || pendingCommand || toolResultReady;
    if (busy || task.active || observationPending || expressionPending) { idleArmed = false; state.idleRemainingMs = idleMs; }
    else {
      if (!idleArmed) { idleArmed = true; idleAt = millis(); }
      const uint32_t elapsed = millis() - idleAt; state.idleRemainingMs = elapsed >= idleMs ? 0 : idleMs - elapsed;
      if (elapsed >= idleMs) terminate = true;
    }
  }
  portEXIT_CRITICAL(&lock);
  if (cancelFace) robot_actions::cancelExpression();
  if (stopAction) { robot_actions::stop("Live connection or action cancelled"); return; }
  if (terminate) { endSession(state.error[0] ? "Live error" : "Conversation ended after inactivity"); return; }
  if (cancelVoice) { reaction_player::cancel(); audioEngine.cancelOutput(); }
  if (command) {
    using C=robot_actions::Command;
    portENTER_CRITICAL(&lock); const bool valid=expectedSession==session && state.liveActive && !dispatchCancelled; portEXIT_CRITICAL(&lock);
    const auto c=dispatchTool.command;
    if (valid && c==C::Stop) { robot_actions::stop("Gemini Stop"); return; }
    bool accepted=false; const char* reason="Conversation cancelled";
    if (valid && robot_actions::isMovement(c)) {
      reason=!task.canMove() ? "No active task or step budget exhausted" : actions.active ? "Another movement is active" : robot_actions::unavailableReason(c);
      if (!reason[0]) accepted=state.dryRun || robot_actions::startFromLive(dispatchTool,controlGeneration);
    } else if (valid && c==C::Expression) {
      reason="Face unavailable or fault has priority";
      accepted=state.dryRun || robot_actions::setExpression(dispatchTool.expression,dispatchTool.durationMs);
    } else if (valid && c==C::StartTask) { accepted=!task.active; reason="Finish the current task first"; }
    else if (valid && c==C::Observe) { accepted=!task.active || task.canObserve(); reason="Observation budget exhausted"; }
    else if (valid && c==C::FinishTask) { accepted=task.active; reason="No active task"; }
    portENTER_CRITICAL(&lock);
    dispatching=false; state.pendingAction=false; pendingCall[0]=0;
    if (accepted && c==C::Expression && !state.dryRun &&
        (expectedSession!=session || !state.liveActive || dispatchCancelled)) stopExpression=true;
    if (accepted && robot_actions::isMovement(c) && !state.dryRun) {
      ownedGeneration=controlGeneration; ownedSession=expectedSession; ownedAction=true;
      snprintf(ownedCall,sizeof(ownedCall),"%s",callId);
      if (expectedSession!=session || !state.liveActive || dispatchCancelled) stopOwned=true;
    }
    if (expectedSession==session && state.liveActive && !dispatchCancelled) {
      if (!accepted) {
        completeTool(callId,reason[0]?reason:"Local tool dispatch failed");
        snprintf(state.reason,sizeof(state.reason),"%.127s",reason);
        if (robot_actions::isMovement(c)) failTask(reason);
      } else if (c==C::StartTask) {
        task.start(millis()); state.taskActive=true; state.taskMoves=0; state.taskResult[0]=0;
        snprintf(state.taskGoal,sizeof(state.taskGoal),"%s",dispatchTool.goal);
        requestObservation(callId,state.dryRun?"test_only_task_started":"task_started");
      } else if (c==C::Observe) {
        if (task.active) ++task.observations;
        requestObservation(callId,"observed");
      } else if (c==C::FinishTask) {
        task.active=state.taskActive=false;
        snprintf(state.taskResult,sizeof(state.taskResult),"%s: %.100s",dispatchTool.outcome,dispatchTool.reason);
        snprintf(state.taskPhase,sizeof(state.taskPhase),"Finished");
        completeTool(callId,state.taskResult); snprintf(state.reason,sizeof(state.reason),"%.127s",state.taskResult);
      } else if (c==C::Expression) {
        if (state.dryRun) completeTool(callId,"test_only_no_expression_started");
        else { expressionPending=true; expressionAt=millis(); snprintf(expressionCall,sizeof(expressionCall),"%s",callId); }
      } else if (state.dryRun) {
        task.moved(false); state.taskMoves=task.moves;
        requestObservation(callId,"test_only_no_movement");
        if (task.noProgress>=task.maxNoProgress) failTask("Test only: no movement progress");
      } else { snprintf(state.taskPhase,sizeof(state.taskPhase),"Moving"); snprintf(state.reason,sizeof(state.reason),"Moving; waiting for local result"); }
      responseProgressAt=millis(); idleArmed=false; state.lastCommand=command;
    }
    portEXIT_CRITICAL(&lock);
  }
}
Status snapshot() {
  portENTER_CRITICAL(&lock); auto copy = state; portEXIT_CRITICAL(&lock);
  const auto settings = gemini_client::settings(); copy.ready = copy.ready && settings.configured;
  if (!copy.liveActive) copy.dryRun = settings.dryRun;
  if (!settings.configured && !copy.error[0]) snprintf(copy.error, sizeof(copy.error), "Add a Gemini API key in the dashboard");
  return copy;
}
}  // namespace speech_commands
