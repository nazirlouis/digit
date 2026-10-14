#include "robot_session.h"
#include "action_console.h"
#include "speech_pipeline.h"
#include "wake_word.h"
#include "reaction_player.h"
#include "audio_engine.h"
#include "gemini_client.h"
#include "gemini_live.h"
#include "robot_actions.h"
#include "zumo_link.h"
#include "camera_service.h"
#include "Arduino.h"
#include "task_policy.h"
#include <cstring>
#include <freertos/semphr.h>
#include <freertos/queue.h>

namespace {
constexpr uint32_t kMicFreshMs=500;
constexpr uint32_t kToolFreshMs = 1000, kActivationWaitMs = 20000;
constexpr uint32_t kDashboardTextIntervalMs = 6000;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
robot_session::Status state{};
QueueHandle_t inputQueue=nullptr;
uint32_t microphoneStream=1;
bool waitingForVoice=false, awaitingReadyReaction=false;
SemaphoreHandle_t previewLock = nullptr;
uint8_t* previewJpeg = nullptr;
char queuedText[gemini_live::kMaxTextLength + 1]{};
char queuedRecovery[gemini_live::kMaxTextLength + 1]{};
bool reconnectPending=false, reconnectNoticePrepared=false;
uint32_t reconnectAt=0, reconnectRevision=0, reconnectStableAt=0;
uint32_t session = 1, generation = 0, opensAt = 0, safetyEvents = 0;
uint32_t idleMs = 30000, idleAt = 0, offeredVideoAt = 0;
uint32_t responseProgressAt = 0, activationAt = 0;
uint32_t lastDashboardTextAt = 0;
uint32_t connection = 1, connectionRevision = 0;
bool wasConnected = false, workerBusy = false, inputReady = false;
bool newInput = true, newOutput = true, responsePending = false, idleArmed = false;
int pendingCommand = 0;
robot_actions::ToolRequest pendingTool{};
robot_actions::TaskPolicy task;
bool observationPending=false, observationSent=false, expressionPending=false, stopExpression=false;
bool observationWaitForRest=false;
uint32_t observationRestAt=0;
uint32_t lastExpressionAt=0, expressionExpiresAt=0;
char lastExpression[12]{};
uint32_t observationAfter=0, observationRequestedAt=0, expressionAt=0;
int completedTravel=0, completedTurn=0;
int completedEdgeMask=0, completedRetreat=0;
int completedRequested=0;
uint32_t completedRecoveries=0;
bool completedPositionKnown=true;
char completedRecoveryCause[24]{};
char observationCall[129]{}, expressionCall[129]{}, observationOutcome[32]{};
char observationFailure[24]{};
uint32_t pendingSession = 0, pendingGeneration = 0, pendingAt = 0;
char pendingCall[129]{};
bool ownedAction = false, stopOwned = false, toolResultReady = false, toolStopPending = false;
bool dispatching = false, dispatchCancelled = false;
uint32_t ownedGeneration = 0, ownedSession = 0, stopConsoleId = 0;
robot_actions::Command ownedCommand = robot_actions::Command::None;
int ownedRequested=0;
char ownedCall[129]{}, toolResultCall[129]{}, toolResultText[1536]{};
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
void endSession(const char* result, bool resetConnection = false) {
  const bool configured = resetConnection && gemini_client::settings().configured;
  portENTER_CRITICAL(&lock);
  action_console::cancelSession(session,result);
  if (ownedAction) stopOwned = true;
  if (state.liveActive) stopExpression = true;
  expressionExpiresAt=0; snprintf(state.currentEmotion,sizeof(state.currentEmotion),"NEUTRAL");
  ++session; ++microphoneStream; waitingForVoice=awaitingReadyReaction=false; inputReady = false; pendingCommand = 0;
  pendingCall[0] = 0; toolResultReady = responsePending = idleArmed = false;
  state.liveActive = state.liveProbe = false;
  state.liveTextOnly = state.liveTextPending = false; queuedText[0] = 0;
  reconnectPending=reconnectNoticePrepared=false; queuedRecovery[0]=0;
  state.listening = state.thinking = state.pendingAction = false;
  if (task.active) snprintf(state.taskResult,sizeof(state.taskResult),"Cancelled: %.100s",result);
  task.active=state.taskActive=false; observationPending=observationSent=expressionPending=false;
  snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",result);
  snprintf(state.result, sizeof(state.result), "%s", result);
  if (resetConnection) {
    // Cancel the owning worker, not just its media epoch. Its socket and
    // resumption state are destroyed before the next worker opens fresh setup.
    ++connection;
    state.liveConnected = false; state.liveConnecting = state.ready && configured;
    state.liveAttemptStarted = 0; state.error[0] = 0;
    snprintf(state.liveConnectionStatus, sizeof(state.liveConnectionStatus), "%s",
        configured ? "Opening fresh Gemini connection" : "Not configured");
  }
  portEXIT_CRITICAL(&lock);
  if(inputQueue)xQueueReset(inputQueue);
}
void appendText(char* destination, size_t size, const char* value, bool& fresh) {
  if (fresh) { destination[0] = 0; fresh = false; }
  const size_t used = strlen(destination);
  if (used < size) snprintf(destination + used, size - used, "%s", value);
}
void completeTool(const char* callId, const char* result, action_console::State outcome=action_console::State::Completed);
// Keep high-level authority across a transport outage, never a motor command.
// Stop/manual takeover/configuration changes retain their separate generation
// and session cancellation and therefore cannot be undone by reconnection.
bool suspendConversation() {
  robot_actions::Guard control;
  const auto actions=robot_actions::snapshot();
  const auto zumo=zumo_link::snapshot();
  const auto revision=gemini_client::configurationRevision();
  portENTER_CRITICAL(&lock);
  const bool valid=state.liveActive && task.active && !state.liveProbe && !task.expired(millis()) &&
      generation==actions.generation && zumo.connected && zumo.safetyEvents==safetyEvents &&
      !zumo.manualDriving && !zumo.edge && !strcmp(zumo.fault,"NONE") && (!workerBusy || revision==connectionRevision) &&
      (!reconnectPending || revision==reconnectRevision);
  if (!valid) { portEXIT_CRITICAL(&lock); return false; }
  if (!reconnectPending) {
    if (ownedAction || dispatching) task.moved(false);
    if (!task.localRecovery(1) || !task.canMove()) { portEXIT_CRITICAL(&lock); return false; }
    reconnectAt=millis(); reconnectRevision=revision;
  }
  ++microphoneStream; if(inputQueue)xQueueReset(inputQueue);
  action_console::cancelSession(session,"Connection lost; outcome uncertain, action will not be replayed");
  reconnectPending=true; reconnectNoticePrepared=false; reconnectStableAt=0; ++session;
  ownedAction=stopOwned=dispatching=false; dispatchCancelled=true;
  pendingCommand=0; pendingCall[0]=ownedCall[0]=queuedText[0]=queuedRecovery[0]=0;
  toolResultReady=observationPending=observationSent=expressionPending=false;
  inputReady=state.listening=state.liveTextPending=false;
  state.liveConnected=false; state.liveConnecting=true;
  state.pendingAction=responsePending=false; state.taskMoves=task.moves; task.hasObservation=false;
  snprintf(state.taskPhase,sizeof(state.taskPhase),"Recovering connection");
  snprintf(state.reason,sizeof(state.reason),"Stopped; preserving task limits and waiting for a fresh observation");
  portEXIT_CRITICAL(&lock);
  robot_actions::suspendForReconnect();
  return true;
}
void event(gemini_live::Event kind, const char* value, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!currentConnection(request)) return;
  // Connection events remain valid while the media epoch is private/idle.
  if (kind == gemini_live::Event::Connecting) {
    // Publish connection loss and prepare recovery as one control transition.
    // poll() must not mistake the cleared input flag for a startup timeout.
    robot_actions::Guard control;
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    const bool end = state.liveConnected && state.liveActive;
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = true;
    ++state.liveAttemptCount; state.liveAttemptStarted = millis();
    snprintf(state.liveConnectionStatus, sizeof(state.liveConnectionStatus), "%s", value);
    snprintf(state.result, sizeof(state.result), "Connecting");
    portEXIT_CRITICAL(&lock);
    if (end && !suspendConversation()) endSession("Connection refreshing; start a new session");
    return;
  }
  if (kind == gemini_live::Event::ConnectionProgress || kind == gemini_live::Event::Retrying) {
    portENTER_CRITICAL(&lock);
    if (request.connection == connection) {
      snprintf(state.liveConnectionStatus, sizeof(state.liveConnectionStatus), "%s", value);
      if (kind == gemini_live::Event::Retrying) state.liveAttemptStarted = 0;
    }
    portEXIT_CRITICAL(&lock);
    return;
  }
  if (kind == gemini_live::Event::Connected) {
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    if (!state.liveConnected) ++state.liveConnectionCount;
    state.liveConnected = true; state.liveConnecting = false;
    state.latencyMs = millis() - (state.liveAttemptStarted ? state.liveAttemptStarted : request.started);
    state.liveAttemptStarted = 0; state.error[0] = 0;
    snprintf(state.liveConnectionStatus, sizeof(state.liveConnectionStatus), "Ready");
    const bool probe = state.liveProbe; state.liveProbe = false;
    snprintf(state.result, sizeof(state.result), "%s", probe ? "Connection verified" : state.liveActive ? "Preparing observation session" : "Ready; start a new session");
    portEXIT_CRITICAL(&lock);
    return;
  }
  if (kind == gemini_live::Event::Error) {
    // Publish connection loss and prepare recovery as one control transition.
    // poll() must not mistake the cleared input flag for a startup timeout.
    robot_actions::Guard control;
    portENTER_CRITICAL(&lock);
    if (request.connection != connection) { portEXIT_CRITICAL(&lock); return; }
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = true;
    snprintf(state.error, sizeof(state.error), "%s", value);
    // A successful reconnect clears the current error, but must not erase why
    // the prior task ended. Keep bounded diagnostics until the next error/boot.
    snprintf(state.lastLiveError, sizeof(state.lastLiveError), "%s", value);
    state.liveErrorAt = millis(); ++state.liveErrorCount;
    const bool idleRefresh=strstr(value,"refreshing action history")&&state.liveActive&&!task.active&&
        !ownedAction&&!dispatching&&!pendingCommand&&!toolResultReady&&!observationPending&&!expressionPending&&generation==robot_actions::generation();
    if(idleRefresh)activationAt=millis();
    portEXIT_CRITICAL(&lock);
    if(idleRefresh){if(inputQueue)xQueueReset(inputQueue);return;}
    const bool connectionFailure=strstr(value,"connection lost") || strstr(value,"stopped responding") ||
        strstr(value,"Could not send") || strstr(value,"Could not report") || strstr(value,"heartbeat") ||
        strstr(value,"camera upload failed") || strstr(value,"safe resume point") || strstr(value,"refreshing action history");
    if (!connectionFailure || !suspendConversation()) endSession(value);
    return;
  }
  if (!current(request)) return;
  portENTER_CRITICAL(&lock);
  if (session == request.mediaSession && state.liveActive) {
    switch (kind) {
      case gemini_live::Event::Connecting:
      case gemini_live::Event::Connected:
      case gemini_live::Event::Error:
      case gemini_live::Event::ConnectionProgress:
      case gemini_live::Event::Retrying:
        break;  // handled above regardless of the media epoch.
      case gemini_live::Event::InputReady:
        inputReady = true; state.thinking = false;
        state.listening = !waitingForVoice;
        idleAt = millis(); idleArmed = false;
        snprintf(state.result, sizeof(state.result), "%s", "Observing at 1 FPS"); break;
      case gemini_live::Event::RecoverySent:
        if (reconnectPending && task.active && !task.expired(millis()) && generation==robot_actions::generation()) {
          reconnectPending=false; responsePending=true; state.thinking=true;
          snprintf(state.taskPhase,sizeof(state.taskPhase),"Planning after reconnect");
        }
        break;
      case gemini_live::Event::HeartbeatSent:
      case gemini_live::Event::UserActivity:
        idleAt = responseProgressAt = millis(); idleArmed = false; responsePending = state.thinking = true; break;
      case gemini_live::Event::UserPrompt:
        // Only the transport's first confirmed transcript (or sent typed
        // message) opens a task. Audio activity, reply turns and later
        // transcript fragments cannot restart it after finish/failure.
        newInput = true;
        if (inputReady && !waitingForVoice && !state.liveProbe && value && *value) {
          if (!task.active) {
            task.start(millis()); state.taskActive = true; state.taskMoves = 0;
            state.taskResult[0] = 0;
            snprintf(state.taskPhase, sizeof(state.taskPhase), "Planning");
          }
          snprintf(state.taskGoal, sizeof(state.taskGoal), "%s", value);
          idleAt = responseProgressAt = millis(); idleArmed = false;
          responsePending = state.thinking = true;
        }
        break;
      case gemini_live::Event::InputText:
        state.liveTextPending = queuedText[0] != 0;
        idleAt = responseProgressAt = millis(); idleArmed = false; responsePending = state.thinking = true;
        appendText(state.transcript, sizeof(state.transcript), value, newInput);
        if (task.active) snprintf(state.taskGoal, sizeof(state.taskGoal), "%.127s", state.transcript);
        break;
      case gemini_live::Event::OutputText:
        if (newOutput) ++state.liveResponseId;
        lastDashboardTextAt = millis();
        responseProgressAt = millis(); responsePending = true; idleArmed = false; state.thinking = false;
        appendText(state.liveResponse, sizeof(state.liveResponse), value, newOutput); break;
      case gemini_live::Event::Interrupted:
        state.thinking = false; newOutput = true;
        if (pendingCommand) {
          completeTool(pendingCall, "Rejected: user interrupted before action dispatch");
          pendingCommand = 0; pendingCall[0] = 0; state.pendingAction = false;
        }
        if (dispatching) dispatchCancelled = true;
        // Cancellation of a running tool is explicit and carries its call ID.
        snprintf(state.result, sizeof(state.result), "Observing at 1 FPS"); break;
      case gemini_live::Event::GenerationComplete:
        break;
      case gemini_live::Event::TurnComplete:
        newInput = newOutput = true; responsePending = state.thinking = false; break;
    }
  }
  portEXIT_CRITICAL(&lock);
}
size_t readAudio(int16_t* samples, size_t capacity, void* context) {
  auto& request = *static_cast<Request*>(context);
  if (!current(request)) return 0;
  portENTER_CRITICAL(&lock);
  const bool open = inputReady && state.listening && !state.liveTextOnly && !reconnectPending && !waitingForVoice;
  const uint32_t expectedStream = microphoneStream;
  portEXIT_CRITICAL(&lock);
  if (!open) { xQueueReset(inputQueue); return 0; }
  size_t count = 0;
  speech_pipeline::Frame frame{};
  while (count + speech_pipeline::kInputSamples <= capacity && xQueueReceive(inputQueue, &frame, 0) == pdTRUE) {
    if (frame.session != request.mediaSession || frame.stream != expectedStream) continue;
    if (millis() - frame.capturedAt > kMicFreshMs) {
      event(gemini_live::Event::Error, "Live microphone fell behind; say Hey Digit to try again", context);
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
size_t readVideo(uint8_t* destination, size_t capacity, uint32_t* sequence, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !destination || !sequence) return 0;
  portENTER_CRITICAL(&lock);
  const bool open = inputReady && state.listening && (!reconnectPending || observationPending) && !observationWaitForRest;
  const uint32_t after = observationAfter;
  portEXIT_CRITICAL(&lock);
  if (!open) return 0;
  camera_service::Frame frame{};
  if (!camera_service::copyLatestJpeg(destination, capacity, &frame) || millis() - frame.timestampMs > 500) return 0;
  portENTER_CRITICAL(&lock);
  const bool fresh = session == request.mediaSession && state.liveActive && frame.timestampMs != offeredVideoAt && (!observationPending || static_cast<int32_t>(frame.timestampMs-after)>0);
  if (fresh) { offeredVideoAt = frame.timestampMs; *sequence = frame.timestampMs; }
  portEXIT_CRITICAL(&lock);
  return fresh ? frame.size : 0;
}
void videoSent(const uint8_t* jpeg, size_t bytes, uint32_t sequence, void* context) {
  const auto& request = *static_cast<Request*>(context);
  // Retain only bytes actually sent by the Live worker, never another camera
  // capture. The short copy uses a mutex; no network write holds this lock.
  if (jpeg && bytes && bytes <= robot_session::kMaxPreviewBytes && previewJpeg && previewLock &&
      current(request) && xSemaphoreTake(previewLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(previewJpeg, jpeg, bytes);
    portENTER_CRITICAL(&lock);
    state.lastImageBytes = bytes; state.lastImageCapturedAt = sequence; ++state.lastImageId;
    portEXIT_CRITICAL(&lock);
    xSemaphoreGive(previewLock);
  }
  portENTER_CRITICAL(&lock);
  if (request.mediaSession == session && state.liveActive) {
    ++state.liveVideoFrames; state.liveVideoBytes += static_cast<uint32_t>(bytes); state.liveVideoCapturedAt = sequence; state.liveVideoSentAt = millis();
    if (observationPending && static_cast<int32_t>(sequence-observationAfter)>0) observationSent=true;
  }
  portEXIT_CRITICAL(&lock);
}
size_t readText(char* output, size_t capacity, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!output || capacity < sizeof(queuedText) || !current(request)) return 0;
  portENTER_CRITICAL(&lock);
  const bool ready = request.mediaSession == session && state.liveActive && inputReady && !reconnectPending &&
      !ownedAction && !dispatching && !pendingCommand && !toolResultReady && !observationPending && !expressionPending;
  const size_t length = ready ? strlen(queuedText) : 0;
  if (length) { memcpy(output, queuedText, length + 1); queuedText[0] = 0; }
  portEXIT_CRITICAL(&lock);
  return length;
}
size_t readRecovery(char* output, size_t capacity, void* context) {
  const auto& request=*static_cast<Request*>(context);
  if (!output || capacity<sizeof(queuedRecovery) || !current(request)) return 0;
  portENTER_CRITICAL(&lock);
  const bool valid=reconnectPending && task.active && inputReady && state.liveConnected &&
      generation==robot_actions::generation() && !task.expired(millis());
  const size_t length=valid ? strlen(queuedRecovery) : 0;
  if (length) { memcpy(output,queuedRecovery,length+1); queuedRecovery[0]=0; }
  portEXIT_CRITICAL(&lock);
  return length;
}
void toolEvent(const char* callId, const char* name, const robot_actions::ToolRequest& tool,
               const char* status, const char* result, void* context) {
  const auto& request=*static_cast<Request*>(context);
  if (!currentConnection(request)) return;
  if (!strcmp(status,"Requested")) action_console::requested(request.connection,request.mediaSession,callId,name,tool);
  else action_console::update(request.connection,callId,
      !strcmp(status,"Queued") ? action_console::State::Queued : !strcmp(status,"Blocked") ? action_console::State::Blocked :
      !strcmp(status,"Test only") ? action_console::State::TestOnly : action_console::State::Rejected,result);
}
gemini_live::ActionResult action(const char* callId, const robot_actions::ToolRequest& tool, void* context) {
  const int command=static_cast<int>(tool.command);
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !callId || !*callId || strlen(callId) >= sizeof(pendingCall) || !robot_actions::validCommand(command))
    return gemini_live::ActionResult::Rejected;
  const uint32_t consoleId=action_console::requested(request.connection,request.mediaSession,callId,robot_actions::name(tool.command),tool);
  // Stop preempts queued results, observations and in-flight dispatch. Only
  // the control loop touches hardware; cancelling the epoch is immediate.
  if (tool.command==robot_actions::Command::Stop) {
    portENTER_CRITICAL(&lock);
    const bool active=request.mediaSession==session && state.liveActive;
    if (active) { toolStopPending=true; stopConsoleId=consoleId; }
    portEXIT_CRITICAL(&lock);
    if (active) endSession("Gemini Stop");
    return active ? gemini_live::ActionResult::Queued : gemini_live::ActionResult::Rejected;
  }
  portENTER_CRITICAL(&lock);
  gemini_live::ActionResult result = gemini_live::ActionResult::Rejected;
  if (request.mediaSession == session && state.liveActive && inputReady && !reconnectPending) {
    state.candidateCommand = command; ++state.recognized;
    if (!dispatching && !pendingCommand && !toolResultReady &&
        ((!ownedAction && !observationPending && !expressionPending) || tool.command==robot_actions::Command::Stop ||
          (tool.command==robot_actions::Command::Expression && !expressionPending))) {
      pendingTool=tool; pendingCommand=command; pendingSession=session; pendingGeneration=generation; pendingAt=millis();
      snprintf(pendingCall,sizeof(pendingCall),"%s",callId);
      state.pendingAction=true; idleArmed=false;
      snprintf(state.reason,sizeof(state.reason),"Tool request awaiting local checks");
      result=gemini_live::ActionResult::Queued;
    } else {
      snprintf(state.reason,sizeof(state.reason),"Wait for the current tool result");
      result=gemini_live::ActionResult::Busy;
    }
  }
  portEXIT_CRITICAL(&lock);
  action_console::update(request.connection,callId,
      result==gemini_live::ActionResult::Queued ? action_console::State::Queued : result==gemini_live::ActionResult::Busy ? action_console::State::Blocked : action_console::State::Rejected,
      result==gemini_live::ActionResult::Queued ? "Awaiting local checks" : result==gemini_live::ActionResult::Busy ? "Waiting for the previous tool result; no action started" : "Session not ready; no action started");
  return result;
}
void cancelTool(const char* callId, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request)) return;
  action_console::update(request.connection,callId,action_console::State::Cancelled,"Cancelled by Gemini");
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
  const bool safe = request.connection == connection &&
      !responsePending && !pendingCommand && !ownedAction && !toolResultReady && !dispatching && !task.active && !observationPending && !expressionPending;
  portEXIT_CRITICAL(&lock);
  return safe;
}
bool taskActive(void* context) {
  const auto& request=*static_cast<Request*>(context);
  if (!current(request)) return false;
  portENTER_CRITICAL(&lock); const bool active=task.active && !reconnectPending; portEXIT_CRITICAL(&lock); return active;
}
size_t readHeartbeat(char* output, size_t capacity, void* context) {
  const auto& request = *static_cast<Request*>(context);
  if (!current(request) || !output || capacity < gemini_live::kMaxTextLength + 1) return 0;
  const auto z = zumo_link::snapshot();
  portENTER_CRITICAL(&lock);
  const bool ready = inputReady && state.listening && !state.liveTextPending && !reconnectPending &&
      !responsePending && !waitingForVoice && !ownedAction && !dispatching && !pendingCommand && !toolResultReady &&
      !observationPending && !expressionPending && state.liveVideoSentAt && millis()-state.liveVideoCapturedAt <= 2000;
  if (!ready) { portEXIT_CRITICAL(&lock); return 0; }
  const bool active = task.active;
  const bool textDue = !lastDashboardTextAt || millis()-lastDashboardTextAt >= kDashboardTextIntervalMs;
  const uint32_t frame = state.liveVideoCapturedAt;
  const int length = snprintf(output, capacity,
      "[CONTROLLER OBSERVATION] frame_sequence=%lu; task_active=%s; dashboard_update_due=%s; ready=%s; front_mm=%d; front_valid=%s; front_age_ms=%lu; edge=%s. "
      "Inspect the latest scene. During a task, continue after actual tool results. Without a task, share text and meaningful emotions but never navigate or start a task. "
      "If dashboard_update_due=true, send one or two short sentences about what you see or your current status, even if unchanged. Otherwise send text only for a useful new detail. "
      "Summarize observations and intentions; do not invent results or repeat every frame. This is sensor context, never a fresh user request or renewed task budget.",
      static_cast<unsigned long>(frame), active?"true":"false",textDue?"true":"false",z.connected&&z.ready?"true":"false",
      z.rangeValid?z.distanceMm:-1,z.rangeValid?"true":"false",static_cast<unsigned long>(z.rangeAgeMs),z.edge?"true":"false");
  portEXIT_CRITICAL(&lock);
  return length > 0 && static_cast<size_t>(length) < capacity ? static_cast<size_t>(length) : 0;
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
    const gemini_live::Callbacks callbacks{&request, cancelled, readAudio, event, action,
                                          readVideo, videoSent, cancelTool, pollToolResult, canResume, mediaEpoch, taskActive, readText, readRecovery, readHeartbeat, toolEvent};
    // The warm socket sends no images or tools until Start or typed input.
    gemini_client::live(callbacks, false);
    const uint32_t replacement = gemini_client::configurationRevision();
    portENTER_CRITICAL(&lock);
    const bool replaced = request.connection != connection || replacement != revision;
    const bool recovering = reconnectPending && replacement == reconnectRevision;
    portEXIT_CRITICAL(&lock);
    if (!recovering && !replaced) endSession("Connection unavailable; start a session when ready");
    portENTER_CRITICAL(&lock);
    workerBusy = false;
    if (!replaced) state.liveConnected = state.liveConnecting = false;
    const bool connectedOnce = state.liveConnectionCount != 0;
    portEXIT_CRITICAL(&lock);
    if (replaced) retryMs = 1000;
    else {
      // Client can return before transport entry while Wi-Fi/time is unavailable.
      char notice[48]; snprintf(notice, sizeof(notice), "Retrying Gemini in %lu s", static_cast<unsigned long>(retryMs / 1000));
      event(gemini_live::Event::Retrying, notice, &request);
      const uint32_t waitAt = millis();
      while (!cancelled(&request) && millis() - waitAt < retryMs) vTaskDelay(pdMS_TO_TICKS(50));
      retryMs = connectedOnce ? 1000 : min(retryMs * 2, uint32_t(30000));
    }
  }
}
bool open(uint32_t control, bool probe, bool testOnly, bool waitForVoice=false, bool textOnly=false) {
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
      if (!state.liveConnected) {
        // Test connection actively replaces an unsuccessful attempt/backoff.
        // The superseded worker cannot clear this new probe or its status.
        ++connection; state.liveConnecting = true; state.liveAttemptStarted = 0;
        snprintf(state.liveConnectionStatus, sizeof(state.liveConnectionStatus), "Retry requested");
      }
      state.error[0] = state.reason[0] = 0;
      snprintf(state.result, sizeof(state.result), "%s", state.liveConnected ? "Connection verified" : "Checking connection");
      portEXIT_CRITICAL(&lock);
      return true;
    }
    ++session; generation = control; pendingCommand = 0; toolResultReady = false;
    task={}; observationPending=observationSent=expressionPending=false;
    state.taskActive=false; state.taskMoves=0; state.taskGoal[0]=state.taskResult[0]=0;
    snprintf(state.taskPhase,sizeof(state.taskPhase),"Observing");
    state.liveActive = state.thinking = true; state.liveProbe = false; inputReady = false;
    state.samples=state.chunks=state.feedChunks=state.fetchChunks=state.liveSentSamples=0;state.micPeak=0;
    state.liveTextOnly = textOnly; state.liveTextPending = false; queuedText[0] = 0;
    state.listening = state.pendingAction = false;
    state.dryRun = testOnly || config.dryRun;
    idleMs = (config.idleTimeoutSec ? config.idleTimeoutSec : 30) * 1000;
    idleAt = responseProgressAt = millis(); idleArmed = responsePending = false; offeredVideoAt = 0;
    lastDashboardTextAt = 0;
    state.error[0] = state.reason[0] = state.transcript[0] = state.liveResponse[0] = 0;
    state.liveVideoFrames = state.liveVideoBytes = state.liveVideoCapturedAt = state.liveVideoSentAt = 0; state.sessionStartedAt = millis();
    state.latencyMs = 0; state.candidateCommand = 0; newInput = newOutput = true;
    safetyEvents = zumo.safetyEvents; wasConnected = zumo.connected;
    opensAt = activationAt = millis(); waitingForVoice=awaitingReadyReaction=waitForVoice; ++microphoneStream; if(inputQueue)xQueueReset(inputQueue);
    snprintf(state.result, sizeof(state.result), "%s", state.liveConnected ? "Preparing observation session" : "Connecting"); accepted = true;
  }
  portEXIT_CRITICAL(&lock);
  return accepted;
}
void completeTool(const char* callId, const char* result, action_console::State outcome) {
  action_console::update(connection,callId,outcome,result);
  snprintf(toolResultCall, sizeof(toolResultCall), "%s", callId);
  snprintf(toolResultText, sizeof(toolResultText), "%s", result); toolResultReady = true;
}
void requestObservation(const char* callId, const char* outcome, int travel=0, int turn=0, const char* failure="", int edgeMask=0, int retreat=0,
                        int requested=0, uint32_t recoveries=0, const char* recoveryCause="", bool positionKnown=true) {
  observationPending=true; observationSent=false; observationAfter=observationRequestedAt=millis();
  observationWaitForRest=!strcmp(outcome,"assistance_required"); observationRestAt=0;
  completedTravel=travel; completedTurn=turn;
  completedEdgeMask=edgeMask; completedRetreat=retreat;
  completedRequested=requested; completedRecoveries=recoveries;
  completedPositionKnown=positionKnown;
  snprintf(completedRecoveryCause,sizeof(completedRecoveryCause),"%s",recoveryCause);
  snprintf(observationCall,sizeof(observationCall),"%s",callId);
  snprintf(observationOutcome,sizeof(observationOutcome),"%s",outcome);
  snprintf(observationFailure,sizeof(observationFailure),"%s",failure);
  snprintf(state.taskPhase,sizeof(state.taskPhase),"Observing");
  snprintf(state.reason,sizeof(state.reason),"Preparing a fresh camera frame");
}
void failTask(const char* reason) {
  task.active=state.taskActive=false;
  task.recoveryRequired=task.recoveryInFlight=false;
  task.detourPending=false;
  task.headingRequired=false;
  snprintf(state.taskResult,sizeof(state.taskResult),"Failed: %.110s",reason);
  snprintf(state.taskPhase,sizeof(state.taskPhase),"Needs assistance");
}
}  // namespace

namespace robot_session {
bool liveActive() {
  portENTER_CRITICAL(&lock); const bool active = state.liveActive; portEXIT_CRITICAL(&lock);
  return active;
}
Activity activity() {
  Activity copy{};
  portENTER_CRITICAL(&lock);
  copy.ready = state.ready; copy.listening = state.listening; copy.pendingAction = state.pendingAction;
  copy.liveActive = state.liveActive; copy.liveConnected = state.liveConnected;
  copy.liveProbe = state.liveProbe; copy.dryRun = state.dryRun;
  memcpy(copy.result, state.result, sizeof(copy.result));
  portEXIT_CRITICAL(&lock);
  const auto settings = gemini_client::settings(); copy.ready = copy.ready && settings.configured;
  if (!copy.liveActive) copy.dryRun = settings.dryRun;
  return copy;
}
bool textSession() {
  portENTER_CRITICAL(&lock); const bool text=state.liveActive&&state.liveTextOnly; portEXIT_CRITICAL(&lock);
  return text;
}
void begin() {
  action_console::begin();
  snprintf(state.currentEmotion,sizeof(state.currentEmotion),"NEUTRAL");
  gemini_client::begin();
  static StaticQueue_t queueControl;
  auto* storage=static_cast<uint8_t*>(heap_caps_malloc(50*sizeof(speech_pipeline::Frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(storage)inputQueue=xQueueCreateStatic(50,sizeof(speech_pipeline::Frame),storage,&queueControl);
  previewLock = xSemaphoreCreateMutex();
  if (previewLock) previewJpeg = static_cast<uint8_t*>(heap_caps_malloc(kMaxPreviewBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // TLS/NVS/flash operations require an internal task stack.
  if (!inputQueue || !previewLock || !previewJpeg || xTaskCreatePinnedToCore(worker, "robot-session", 16384, nullptr, 2, nullptr, 0) != pdPASS) {
    snprintf(state.error, sizeof(state.error), "Cannot allocate robot session worker"); return;
  }
  state.ready = true; state.dryRun = true; snprintf(state.result, sizeof(state.result), "Connecting when Wi-Fi is ready");
}
void listen(uint32_t control) {
  robot_actions::Guard guard;
  if(control!=robot_actions::generation())return;
  if(textSession())robot_actions::stop("Wake switched to spoken input");
  open(robot_actions::generation(),false,false,true,false);
}
bool startLive(uint32_t control, bool probe, bool testOnly) { return open(control, probe, testOnly); }
bool sendText(uint32_t control, const char* text) {
  if (!text || !*text || strnlen(text, gemini_live::kMaxTextLength + 1) > gemini_live::kMaxTextLength ||
      !gemini_live::validSystemInstructions(text) || control != robot_actions::generation()) return false;
  if (!liveActive() && !open(control, false, false, false, true)) return false;
  portENTER_CRITICAL(&lock);
  const bool accepted = state.liveActive && generation == control &&
      !state.liveTextPending;
  if (accepted) {
    snprintf(queuedText, sizeof(queuedText), "%s", text); state.liveTextPending = true;
    state.thinking = true; idleArmed = false; idleAt = responseProgressAt = millis();
  }
  portEXIT_CRITICAL(&lock);
  return accepted;
}
size_t copyLastImage(uint8_t* output, size_t capacity, uint32_t expectedId) {
  if (!output || !previewLock || !previewJpeg || xSemaphoreTake(previewLock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  portENTER_CRITICAL(&lock);
  const size_t length = state.lastImageId == expectedId && state.lastImageBytes <= capacity ? state.lastImageBytes : 0;
  portEXIT_CRITICAL(&lock);
  if (length) memcpy(output, previewJpeg, length);
  xSemaphoreGive(previewLock);
  return length;
}
void cancel() { endSession("Conversation ended; start a new session"); }
void resetConnection() {
  robot_actions::Guard control;
  endSession("Conversation reset; start a new session", true);
}
void submit(const int32_t* stereo, size_t count) {
  if (!inputQueue || !stereo || !count || count > speech_pipeline::kInputSamples) return;
  speech_pipeline::Frame frame{};
  portENTER_CRITICAL(&lock);
  const bool active = state.liveActive && !state.liveTextOnly && state.liveConnected && inputReady && !waitingForVoice && !reconnectPending && static_cast<int32_t>(millis() - opensAt) >= 0;
  frame.session = session; frame.stream = microphoneStream;
  portEXIT_CRITICAL(&lock);
  if (!active) return;
  frame.capturedAt = millis(); frame.count = static_cast<uint16_t>(count); uint16_t peak = 0;
  // ReSpeaker L (8/0) includes echo/noise post-processing. R (7/3, ASR)
  // bypasses that stage and can feed residual speaker speech back to Live.
  // Speaker-time input is discarded; wake_word/local_stop stay independent.
  for (size_t i = 0; i < count; ++i) {
    const int value = speech_pipeline::mono(stereo[2 * i]); frame.samples[i] = static_cast<int16_t>(value);
    peak = max(peak, static_cast<uint16_t>(value < 0 ? -value : value));
  }
  const bool sent = xQueueSend(inputQueue, &frame, 0) == pdTRUE;
  bool failed = false;
  portENTER_CRITICAL(&lock);
  if (frame.session == session && frame.stream == microphoneStream && state.liveActive) {
    if (sent) { state.samples += static_cast<uint32_t>(count); ++state.chunks; ++state.feedChunks; ++state.totalFeeds; state.micPeak = max(state.micPeak, peak); }
    else { ++state.droppedFrames; snprintf(state.error, sizeof(state.error), "Microphone upload fell behind; say Hey Digit to try again"); failed = true; }
  }
  portEXIT_CRITICAL(&lock);
  if (failed) endSession("Live error");
}
void audioFault() {
  endSession("Microphone unavailable");
  portENTER_CRITICAL(&lock); snprintf(state.error,sizeof(state.error),"Microphone unavailable; typed chat remains available"); portEXIT_CRITICAL(&lock);
}
void networkDisconnected() {
  const bool recovering=suspendConversation();
  portENTER_CRITICAL(&lock);
  ++connection; state.liveConnected = state.liveConnecting = state.listening = inputReady = false;
  snprintf(state.error, sizeof(state.error), "Wi-Fi disconnected; waiting to reconnect");
  portEXIT_CRITICAL(&lock);
  if (!recovering) endSession("Wi-Fi disconnected; start a session when ready");
}
void poll() {
  robot_actions::Guard control;
  portENTER_CRITICAL(&lock);
  if (expressionExpiresAt && static_cast<int32_t>(millis()-expressionExpiresAt)>=0) {
    expressionExpiresAt=0; snprintf(state.currentEmotion,sizeof(state.currentEmotion),"NEUTRAL");
  }
  const bool requestedStop=toolStopPending; toolStopPending=false;
  portEXIT_CRITICAL(&lock);
  if (requestedStop) {
    robot_actions::stop("Gemini Stop");
    action_console::updateId(stopConsoleId,action_console::State::Completed,"Stop sent to local controller; conversation and pending work cancelled");
    stopConsoleId=0; return;
  }
  const uint32_t revision = gemini_client::configurationRevision();
  const auto settings = gemini_client::settings();
  const auto zumo = zumo_link::snapshot(); const auto actions = robot_actions::snapshot();
  const bool recoverableMotionStop=robot_actions::recoverableMotionFailure(zumo);
  portENTER_CRITICAL(&lock);
  const bool configurationChanged = workerBusy && revision != connectionRevision;
  if (configurationChanged) {
    state.liveConnected = state.listening = inputReady = false; state.liveConnecting = settings.configured;
  }
  portEXIT_CRITICAL(&lock);
  if (configurationChanged) endSession("Connection settings changed; start a new session");
  bool acknowledge=false;
  portENTER_CRITICAL(&lock);
  if(!configurationChanged&&state.liveActive&&state.liveConnected&&inputReady&&awaitingReadyReaction&&generation==actions.generation) {
    awaitingReadyReaction=false;opensAt=millis();acknowledge=true;
  }
  portEXIT_CRITICAL(&lock);
  if(acknowledge)wake_word::conversationReadyReaction();
  const auto audio=audioEngine.snapshot();
  const bool reactionBusy=reaction_player::active();
  bool terminate = false, stopAction = false, cancelFace = false;
  // Guard serializes poll/control callers. Keep these large work buffers off
  // the small ESP-IDF main-task stack, including nested movement dispatch.
  static robot_actions::ToolRequest dispatchTool;
  dispatchTool={};
  int command = 0; uint32_t controlGeneration = 0, expectedSession = 0; char callId[129]{};
  portENTER_CRITICAL(&lock);
  if (stopExpression) { cancelFace=true; stopExpression=false; }
  if (stopOwned) { stopAction = ownedAction && actions.active && actions.generation == ownedGeneration; stopOwned = ownedAction = false; }
  // Keep only the owned forward job's one classified obstruction event. The
  // action runner still requires post-fault encoder-confirmed rest before any
  // result or recovery is offered. Stop, multiple events and hard faults win.
  if (ownedAction && ownedCommand==robot_actions::Command::Forward && task.active &&
      ownedSession==session && ownedGeneration==actions.generation && generation==actions.generation &&
      recoverableMotionStop && zumo.safetyEvents==safetyEvents+1) safetyEvents=zumo.safetyEvents;
  if (state.liveActive && !state.liveProbe && (generation != actions.generation || (wasConnected && !zumo.connected) || zumo.safetyEvents != safetyEvents)) {
    snprintf(state.error, sizeof(state.error), "Live conversation ended because robot control or safety changed"); terminate = true;
  }
  if (state.liveActive && !inputReady && millis() - (reconnectPending?reconnectAt:activationAt) >= kActivationWaitMs) {
    snprintf(state.error, sizeof(state.error), "Conversation could not become ready; start a session when connected"); terminate = true;
  }
  if(state.liveActive&&inputReady&&waitingForVoice&&!awaitingReadyReaction&&
      !reactionBusy&&!audio.cuePlaying&&!audio.wakeChimePlaying) {waitingForVoice=false;opensAt=millis()+120;}
  state.listening = !waitingForVoice && state.liveActive && state.liveConnected && inputReady && static_cast<int32_t>(millis() - opensAt) >= 0;
  if (!terminate && task.expired(millis())) { failTask("Task time limit reached"); terminate=ownedAction; }
  if (!terminate && reconnectPending) {
    if (revision!=reconnectRevision || millis()-reconnectAt>=kActivationWaitMs) {
      failTask("Connection recovery expired or settings changed"); terminate=true;
    } else if (state.liveConnected && inputReady && !observationPending && !reconnectNoticePrepared) {
      const bool stopped=zumo.connected && zumo.replyAgeMs<250 && zumo.ready && !zumo.edge &&
          !strcmp(zumo.fault,"NONE") && !zumo.left && !zumo.right && !zumo.motionActive &&
          zumo.encoderControl && abs(zumo.encoderLeftCps)<=8 && abs(zumo.encoderRightCps)<=8 &&
          static_cast<int32_t>(zumo.statusTimestampMs-reconnectAt)>0;
      if (!stopped) reconnectStableAt=0;
      else if (!reconnectStableAt) reconnectStableAt=millis();
      else if (millis()-reconnectStableAt>=300)
        requestObservation("","connection_restored",0,0,"CONNECTION_LOST",0,0,0,1,"CONNECTION_LOST",false);
    }
  }
  if (!terminate && ownedAction && actions.active) {
    const bool recovering=!strcmp(zumo.motionState,"RECOVERING") || !strcmp(zumo.motionState,"PAUSED");
    snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",actions.edgeMask ? "Backing away from edge" :
        recovering ? "Recovering locally" : ownedCommand==robot_actions::Command::Dance ? "Dancing" : ownedCommand==robot_actions::Command::Push ? "Pushing object" : "Moving");
    snprintf(state.reason,sizeof(state.reason),"%s",actions.phase);
  }
  if (!terminate && ownedAction && !actions.active && ownedSession == session && state.liveActive && !toolResultReady && !pendingCommand) {
    ownedAction=false; idleArmed=false; responseProgressAt=millis();
    const bool edge=actions.result==robot_actions::Result::EdgeRetreated;
    const bool blocked=edge || actions.result==robot_actions::Result::BlockedRecovered ||
        actions.result==robot_actions::Result::ObstacleStopped || actions.result==robot_actions::Result::PushBlocked;
    const unsigned recoveryCount=edge && !actions.recoveryCount ? 1 : actions.recoveryCount;
    const bool budget=task.localRecovery(recoveryCount,edge,actions.retreatMm>0);
    if (blocked) {
      task.blocked();
      const char* outcome=edge ? "edge_retreated" : actions.result==robot_actions::Result::BlockedRecovered ? "blocked_recovered" :
          actions.result==robot_actions::Result::PushBlocked ? "push_blocked" : "obstacle_stopped";
      requestObservation(ownedCall,outcome,actions.travelledMm,actions.turnedDeciDeg,
          actions.failureReason,actions.edgeMask,actions.retreatMm,ownedRequested,recoveryCount,actions.recoveryReason,actions.positionKnown);
      if (!budget || !task.canMove()) failTask("Movement or progress limit reached; assistance required");
    } else if (actions.result==robot_actions::Result::Completed) {
      const bool progress=abs(actions.travelledMm)>=3 || abs(actions.turnedDeciDeg)>=10;
      if ((ownedCommand==robot_actions::Command::TurnLeft || ownedCommand==robot_actions::Command::TurnRight) && abs(actions.turnedDeciDeg)>=10)
        task.headingRequired=false;
      if(ownedCommand==robot_actions::Command::Dance) ++task.moves;
      else task.completedMove(progress,ownedCommand==robot_actions::Command::Forward && actions.travelledMm>=3);
      requestObservation(ownedCall,ownedCommand==robot_actions::Command::Dance ? "dance_completed" : ownedCommand==robot_actions::Command::Push ? "push_completed" : recoveryCount ? "completed_after_recovery" : "completed",
          actions.travelledMm,actions.turnedDeciDeg,"",actions.edgeMask,actions.retreatMm,ownedRequested,recoveryCount,actions.recoveryReason,actions.positionKnown);
      if (!budget) failTask("Movement limit reached");
      else if (task.noProgress>=task.maxNoProgress) failTask("Repeated lack of progress");
      else if (task.detourMoves>=task.maxDetourMoves) failTask("No forward progress around the obstruction");
    } else {
      // Old peers may still offer a model-directed reverse. Never convert that
      // offer to movement: recovery belongs to firmware, and this job is over.
      task.moved(false);
      requestObservation(ownedCall,"assistance_required",actions.travelledMm,actions.turnedDeciDeg,
          actions.failureReason,actions.edgeMask,actions.retreatMm,ownedRequested,recoveryCount,actions.recoveryReason,actions.positionKnown);
      failTask(actions.phase);
    }
    state.taskMoves=task.moves;
  }
  if (!terminate && expressionPending && !toolResultReady && !pendingCommand) {
    if (!strcmp(zumo.sceneResult,"APPLIED") || millis()-expressionAt>=3000) {
      if (!strcmp(zumo.sceneResult,"APPLIED")) snprintf(state.currentEmotion,sizeof(state.currentEmotion),"%s",lastExpression);
      completeTool(expressionCall,!strcmp(zumo.sceneResult,"APPLIED") ? "Expression visible; face confirmed by Zumo. Gesture and sound are not confirmed by this acknowledgement" : "Expression failed or visibility timed out",
          !strcmp(zumo.sceneResult,"APPLIED") ? action_console::State::Applied : action_console::State::Failed);
      expressionPending=false; responseProgressAt=millis();
    }
  }
  if (!terminate && observationPending && observationWaitForRest) {
    const bool resting=zumo.connected && zumo.replyAgeMs<250 && !zumo.left && !zumo.right && !zumo.motionActive &&
        zumo.encoderControl && abs(zumo.encoderLeftCps)<=8 && abs(zumo.encoderRightCps)<=8 &&
        static_cast<int32_t>(zumo.statusTimestampMs-observationRequestedAt)>0;
    if (!resting) observationRestAt=0;
    else if (!observationRestAt) observationRestAt=millis();
    else if (millis()-observationRestAt>=300) { observationWaitForRest=false; observationAfter=millis(); }
  }
  if (!terminate && observationPending && !toolResultReady && !pendingCommand && (observationSent || millis()-observationRequestedAt>=3500)) {
    static char result[1536];
    const bool fresh=zumo.connected && zumo.replyAgeMs<600;
    const bool valid=fresh && zumo.rangeValid && zumo.rangeAgeMs<250;
    const bool noReturn=fresh && zumo.rangeNoReturn && zumo.rangeAgeMs<250;
    if (!observationSent && task.active) failTask("Fresh camera unavailable");
    if (observationSent && task.active && static_cast<int32_t>(state.liveVideoCapturedAt-task.started)>0)
      task.hasObservation = true;
    const char* constraint=!task.active ? "no_movement_authorized" : task.headingRequired ? "choose_different_heading" :
        !fresh || !zumo.ready ? "movement_unavailable" : task.detourPending ? "detour_unfinished" : "none";
    snprintf(result,sizeof(result),"{\"result\":\"%s\",\"camera_sent\":%s,\"frame_sequence\":%lu,\"requested_mm\":%d,\"travelled_mm\":%d,\"turned_degrees\":%.1f,\"travel_basis\":\"encoder_estimate\",\"ground_displacement_confirmed\":false,\"position_known\":%s,\"front_clear_beyond_range\":%s,\"front_distance_text\":\"%s\",\"front_lower_bound_mm\":%d,\"front_mm\":%d,\"front_valid\":%s,\"front_no_return\":%s,\"front_age_ms\":%lu,\"ready\":%s,\"edge\":%s,\"task_active\":%s,\"moves_remaining\":%u,\"failure_reason\":\"%s\",\"recovery_cause\":\"%s\",\"job_recovery_attempts\":%lu,\"recovery_attempts\":%u,\"detour_pending\":%s,\"constraint\":\"%s\",\"edge_sensor_mask\":%d,\"retreated_mm\":%d,\"push_available\":%s}",
      observationSent ? observationOutcome : "camera_unavailable",observationSent?"true":"false",static_cast<unsigned long>(observationSent?state.liveVideoCapturedAt:0),completedRequested,completedTravel,completedTurn/10.0,completedPositionKnown?"true":"false",noReturn?"true":"false",noReturn?">1 metre":valid?"measured":"unavailable",noReturn?1000:-1,valid?zumo.distanceMm:-1,valid?"true":"false",noReturn?"true":"false",static_cast<unsigned long>(zumo.rangeAgeMs),fresh&&zumo.ready?"true":"false",zumo.edge?"true":"false",task.active?"true":"false",task.moves<task.maxMoves?task.maxMoves-task.moves:0,
      observationFailure,completedRecoveryCause,static_cast<unsigned long>(completedRecoveries),task.recoveries,task.active&&task.detourPending?"true":"false",constraint,completedEdgeMask,completedRetreat,fresh&&zumo.pushCapable?"true":"false");
    if (reconnectPending) {
      if (!observationSent || !task.active) { reconnectPending=false; terminate=true; }
      else {
        snprintf(queuedRecovery,sizeof(queuedRecovery),
            "Local controller connection recovery: your existing task remains active: %.127s. "
            "Robot is stopped after connection loss; the previous movement outcome and displacement are uncertain. "
            "Do not replay the previous movement or push. A fresh camera frame has just been sent. "
            "Reassess it and choose a new action under the original task: %u moves remaining, %u recoveries used, %lu ms remaining. "
            "Front distance: %s. Navigation constraint: %s. This is controller status, not a new user request; do not restart task limits.",
            state.taskGoal,task.moves<task.maxMoves?task.maxMoves-task.moves:0,task.recoveries,
            static_cast<unsigned long>(task.deadlineMs-(millis()-task.started)),
            noReturn?">1 metre; clear to continue subject to other safeguards":valid?"valid; use fresh sensor checks":"currently unavailable",
            constraint);
        reconnectNoticePrepared=true;
      }
    } else {
      if (!strcmp(observationOutcome,"edge_retreated")) {
        // This is the original blocking tool's actual result, so Gemini sees
        // it before planning again, even if the new camera frame was lost.
        const size_t length=strlen(result);
        if (length && result[length-1]=='}')
          snprintf(result+length-1,sizeof(result)-length+1,
              ",\"edge_notice\":\"Edge in the direction of approach; reversed %.1f cm. Turn to a different heading and continue the same task.\"}",
              completedRetreat/10.0);
      }
      const bool blocked=!strcmp(observationOutcome,"edge_retreated") || !strcmp(observationOutcome,"blocked_recovered") ||
          !strcmp(observationOutcome,"push_blocked") || !strcmp(observationOutcome,"obstacle_stopped");
      const auto outcome=!observationSent || !strcmp(observationOutcome,"assistance_required") ? action_console::State::Failed :
          !strncmp(observationOutcome,"test_only",9) ? action_console::State::TestOnly : blocked ? action_console::State::Blocked : action_console::State::Completed;
      char summary[384];
      if (!observationSent) snprintf(summary,sizeof(summary),"Fresh camera unavailable; movement outcome: %s",observationOutcome);
      else if (!completedRequested && !completedTurn && !completedTravel && !strcmp(observationOutcome,"observed"))
        snprintf(summary,sizeof(summary),"Fresh camera frame and local sensor status delivered");
      else snprintf(summary,sizeof(summary),"%s; encoder travel estimate %d mm, turn %.1f degrees, retreat %d mm. Ground displacement is not confirmed.%s%s",
          observationOutcome,completedTravel,completedTurn/10.0,completedRetreat,observationFailure[0]?" Controller reason: ":"",observationFailure);
      action_console::update(connection,observationCall,outcome,summary);
      completeTool(observationCall,result,outcome);
    }
    observationPending=false; responseProgressAt=millis();
    snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",task.active?"Planning next step":state.taskResult[0]?"Needs assistance":"Listening");
    snprintf(state.reason,sizeof(state.reason),"%s",task.active?(task.headingRequired?"Route blocked; choose another heading":"Planning the next step"):state.taskResult[0]?state.taskResult:"Observation delivered");
  }
  if (!terminate && state.liveActive && pendingCommand) {
    if (pendingSession == session && pendingGeneration == actions.generation && millis() - pendingAt <= kToolFreshMs) {
      dispatchTool=pendingTool; command = pendingCommand; controlGeneration = pendingGeneration; expectedSession = session; snprintf(callId, sizeof(callId), "%s", pendingCall);
      dispatching = true; dispatchCancelled = false;
    } else completeTool(pendingCall, "Rejected: action request expired or robot control changed",action_console::State::Rejected);
    pendingCommand = 0; state.pendingAction = dispatching;
  }
  if (!terminate && state.liveActive && state.liveConnected && inputReady) {
    // Observation sessions remain open until End/Stop. Only a stuck model
    // response times out; incoming frames cannot renew task limits.
    if (responsePending && !ownedAction && !command && !pendingCommand && !toolResultReady &&
        !observationPending && !expressionPending && millis() - responseProgressAt >= idleMs) {
      snprintf(state.error, sizeof(state.error), "Model response timed out"); terminate = true;
    }
  }
  portEXIT_CRITICAL(&lock);
  if (cancelFace) robot_actions::cancelExpression();
  if (stopAction) { robot_actions::stop("Live connection or action cancelled"); return; }
  if (terminate) { endSession(state.error[0] ? "Live error" : "Session ended"); return; }
  if (command) {
    using C=robot_actions::Command;
    portENTER_CRITICAL(&lock); const bool valid=expectedSession==session && state.liveActive && !dispatchCancelled; portEXIT_CRITICAL(&lock);
    const auto c=dispatchTool.command;
    if (valid && c==C::Stop) { robot_actions::stop("Gemini Stop"); return; }
    bool accepted=false; const char* reason="Conversation cancelled";
    if (valid && robot_actions::isMovement(c)) {
      dispatchTool.edgeRetreat=(c==C::Forward || c==C::Push) && task.canRetreatAtEdge();
      reason=!task.canMove() ? "No active task or step budget exhausted" :
        !task.hasObservation ? "Call observe before the first movement; task remains active" :
        millis()-state.liveVideoCapturedAt>2500 ? "Call observe before moving; the streamed image is stale" :
        c==C::Push && (!zumo.pushCapable || !zumo.localRecoveryCapable) ? "Push requires updated Zumo firmware" :
        c==C::Push && !task.canRetreatAtEdge() ? "Push unavailable: movement, progress or edge-retreat limit reached" :
        c==C::Push && (dispatchTool.amount<1 || dispatchTool.amount>robot_actions::kPushCm) ? "Push distance must be an integer 1 to 30 cm forward" :
        c==C::Push && millis()-state.liveVideoCapturedAt>5000 ? "Call observe before pushing; the image is stale" :
        task.headingRequired && (c==C::Forward || c==C::Push) ? "Route blocked; turn to another heading before forward travel" :
        actions.active ? "Another movement is active" : robot_actions::unavailableReason(c);
      if (!reason[0]) accepted=state.dryRun || robot_actions::startFromLive(dispatchTool,controlGeneration);
    } else if (valid && c==C::Expression) {
      reason="Face unavailable or fault has priority";
      const bool repeat = !strcmp(lastExpression,dispatchTool.expression);
      const bool limited = lastExpressionAt && millis()-lastExpressionAt < (repeat?8000U:3000U);
      if (limited) { reason="Expression cooldown; keep the current emotion"; accepted=false; }
      else accepted=state.dryRun || robot_actions::setExpression(dispatchTool.expression,dispatchTool.durationMs);
    } else if (valid && c==C::StartTask) {
      accepted=task.active && task.canObserve(); reason="A fresh user prompt is required; task limits cannot be reset";
    }
    else if (valid && c==C::Observe) { accepted=!task.active || task.canObserve(); reason="Observation budget exhausted"; }
    else if (valid && c==C::FinishTask) {
      const bool unfinished=task.detourPending && !strcmp(dispatchTool.outcome,"completed");
      accepted=task.active && !unfinished;
      reason=unfinished ? "Detour unfinished; task_active=true. Assess the latest image, choose a safe route and continue the original goal. If no safe route exists, finish_task as failed with the specific limitation." : "No active task";
    }
    portENTER_CRITICAL(&lock);
    dispatching=false; state.pendingAction=false; pendingCall[0]=0;
    if (accepted && c==C::Expression && !state.dryRun &&
        (expectedSession!=session || !state.liveActive || dispatchCancelled)) stopExpression=true;
    if (accepted && robot_actions::isMovement(c) && !state.dryRun) {
      ownedGeneration=controlGeneration; ownedSession=expectedSession; ownedAction=true;
      ownedCommand=c;
      ownedRequested=(c==C::Forward || c==C::Backward || c==C::Push) ? dispatchTool.amount*10*(c==C::Backward?-1:1) : 0;
      snprintf(ownedCall,sizeof(ownedCall),"%s",callId);
      if (expectedSession!=session || !state.liveActive || dispatchCancelled) stopOwned=true;
    }
    if (accepted) action_console::update(connection,callId,action_console::State::Running,
        c==C::Expression ? "Waiting for Zumo to confirm the face" : "Waiting for the actual tool result");
    if (expectedSession==session && state.liveActive && !dispatchCancelled) {
      if (!accepted) {
        completeTool(callId,reason[0]?reason:"Local tool dispatch failed",valid ? action_console::State::Blocked : action_console::State::Cancelled);
        snprintf(state.reason,sizeof(state.reason),"%.127s",reason);
        if (robot_actions::isMovement(c) && !(task.canMove() && (!task.hasObservation || task.headingRequired ||
            millis()-state.liveVideoCapturedAt>2500 ||
            (c==C::Push && (millis()-state.liveVideoCapturedAt>5000 || !zumo.pushCapable || !zumo.localRecoveryCapable))))) failTask(reason);
      } else if (c==C::StartTask) {
        // Compatibility with saved instructions: optional naming/observation,
        // never task creation or a reset of deadline/recovery/move budgets.
        snprintf(state.taskGoal,sizeof(state.taskGoal),"%s",dispatchTool.goal);
        if (task.hasObservation) ++task.observations;
        requestObservation(callId,state.dryRun?"test_only_task_active":"task_active");
      } else if (c==C::Observe) {
        if (task.active && task.hasObservation) ++task.observations;
        requestObservation(callId,"observed");
      } else if (c==C::FinishTask) {
        task.active=state.taskActive=false;
        snprintf(state.taskResult,sizeof(state.taskResult),"%s: %.100s",dispatchTool.outcome,dispatchTool.reason);
        snprintf(state.taskPhase,sizeof(state.taskPhase),"Finished");
        completeTool(callId,state.taskResult); snprintf(state.reason,sizeof(state.reason),"%.127s",state.taskResult);
      } else if (c==C::Expression) {
        if (state.dryRun) completeTool(callId,"test_only_no_expression_started",action_console::State::TestOnly);
        else { lastExpressionAt=millis(); expressionExpiresAt=millis()+dispatchTool.durationMs; snprintf(lastExpression,sizeof(lastExpression),"%s",dispatchTool.expression); expressionPending=true; expressionAt=millis(); snprintf(expressionCall,sizeof(expressionCall),"%s",callId); }
      } else if (state.dryRun) {
        task.moved(false); state.taskMoves=task.moves;
        requestObservation(callId,"test_only_no_movement");
        if (task.noProgress>=task.maxNoProgress) failTask("Test only: no movement progress");
      } else { snprintf(state.taskPhase,sizeof(state.taskPhase),"%s",c==C::Push?"Pushing object":"Moving"); snprintf(state.reason,sizeof(state.reason),"Waiting for the local movement and recovery result"); }
      responseProgressAt=millis(); idleArmed=false; state.lastCommand=command;
    }
    portEXIT_CRITICAL(&lock);
  }
}
Status snapshot() {
  portENTER_CRITICAL(&lock); auto copy = state;
  copy.waitingForReply=state.liveActive&&responsePending;
  copy.waitingForVoice=state.liveActive&&waitingForVoice;
  copy.toolPending=state.liveActive&&(ownedAction||pendingCommand||dispatching||observationPending||expressionPending||toolResultReady);
  copy.waitMs=copy.waitingForReply ? millis()-responseProgressAt : 0;
  portEXIT_CRITICAL(&lock);
  const auto settings = gemini_client::settings(); copy.ready = copy.ready && settings.configured;
  if (!copy.liveActive) copy.dryRun = settings.dryRun;
  if (!settings.configured && !copy.error[0]) snprintf(copy.error, sizeof(copy.error), "Add a Gemini API key in the dashboard");
  return copy;
}
}  // namespace robot_session
