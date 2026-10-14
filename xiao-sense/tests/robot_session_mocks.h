#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include <string>
#include "gemini_client.h"
#include "robot_session.h"
#include "speech_pipeline.h"

namespace mock {
inline uint32_t now = 1000, generation = 10, safetyEvents = 4;
inline bool connected = true, actionAvailable = true, playing = false, actionActive = false;
inline bool cuePlaying = false, chimePlaying = false, reactionActive = false;
inline bool taskCreationFails = false, queueFull = false, semaphoreBusy = false;
inline unsigned criticalDepth = 0, mutexDepth = 0, controlDepth = 0, networkCalls = 0;
inline unsigned actionAttempts = 0, dispatched = 0, stops = 0, cancelOutputCalls = 0, reactionCancelCalls = 0;
inline unsigned readyReactions = 0;
inline unsigned expressionCancels = 0;
inline uint32_t configurationRevision = 1;
inline bool liveReturns = false;
inline int dispatchedCommand = 0;
inline bool cancelledBefore = false, cancelledAfter = false;
inline gemini_client::Settings settings{true, "gemini-test-model", true, 30};
inline void (*workerTask)(void*) = nullptr;
inline void* queueStorage = nullptr;
inline size_t queueItemSize = 0;
inline std::deque<std::vector<uint8_t>> queued;
inline std::function<void()> feedAudio, beforeStartHook;
inline std::function<void()> afterCriticalExit;
inline std::function<void()> afterControlRelease;
inline std::function<void()> afterMutexGive;
inline std::function<void(const gemini_live::Callbacks&, bool)> liveHook;
struct StopWorker {};
inline void leaveCritical() {
  assert(criticalDepth > 0); --criticalDepth;
  if (!criticalDepth && afterCriticalExit) {
    auto hook = afterCriticalExit; afterCriticalExit = nullptr; hook();
  }
}
}  // namespace mock

using portMUX_TYPE = int;
using StaticQueue_t = int;
using QueueHandle_t = void*;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux), ++mock::criticalDepth)
#define portEXIT_CRITICAL(mux) ((void)(mux), mock::leaveCritical())
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
using std::max;
using std::min;
inline uint32_t millis() { return mock::now; }
inline uint32_t esp_random() { return 123456; }
inline uint32_t pdMS_TO_TICKS(uint32_t value) { return value; }
inline void vTaskDelay(uint32_t) { throw mock::StopWorker{}; }
inline void* heap_caps_malloc(size_t size, int) { return std::malloc(size); }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) {
  if (mock::semaphoreBusy) return 0;
  ++mock::mutexDepth; return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t) {
  assert(mock::mutexDepth > 0); --mock::mutexDepth;
  if (!mock::mutexDepth && mock::afterMutexGive) {
    auto hook = mock::afterMutexGive; mock::afterMutexGive = nullptr; hook();
  }
}
inline QueueHandle_t xQueueCreateStatic(size_t, size_t itemSize, uint8_t* storage, StaticQueue_t*) {
  mock::queueStorage = storage; mock::queueItemSize = itemSize;
  return reinterpret_cast<void*>(1);
}
inline int xQueueSend(QueueHandle_t, const void* item, uint32_t) {
  if (mock::queueFull || mock::queued.size() >= 50) return 0;
  const auto* bytes = static_cast<const uint8_t*>(item);
  mock::queued.emplace_back(bytes, bytes + mock::queueItemSize);
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t, void* destination, uint32_t) {
  assert(mock::criticalDepth == 0);
  if (mock::queued.empty() && mock::feedAudio) mock::feedAudio();
  if (mock::queued.empty()) return 0;
  std::memcpy(destination, mock::queued.front().data(), mock::queueItemSize);
  mock::queued.pop_front();
  return pdTRUE;
}
inline void xQueueReset(QueueHandle_t) { mock::queued.clear(); }
inline int xTaskCreatePinnedToCore(void (*task)(void*), const char*, unsigned,
                                 void*, unsigned, TaskHandle_t*, unsigned) {
  mock::workerTask = task;
  return mock::taskCreationFails ? 0 : pdPASS;
}

namespace gemini_client {
inline void live(const gemini_live::Callbacks& callbacks, bool probe) {
  assert(mock::criticalDepth == 0 && mock::mutexDepth == 0);
  ++mock::networkCalls;
  if (mock::liveHook) mock::liveHook(callbacks, probe);
  if (!mock::liveReturns) throw mock::StopWorker{};
}
inline void begin() {}
inline Settings settings() { return mock::settings; }
inline uint32_t configurationRevision() { return mock::configurationRevision; }
}  // namespace gemini_client

namespace zumo_link {
struct Status { bool connected; uint32_t safetyEvents; bool ready=true,rangeValid=true,rangeNoReturn=false,edge=false,distanceCalibrated=true; int distanceMm=500; uint32_t replyAgeMs=0,rangeAgeMs=0; char sceneResult[24]="APPLIED";
  bool localRecoveryCapable=true,pushCapable=true,encoderControl=true,motionActive=false,manualDriving=false;
  int left=0,right=0,encoderLeftCps=0,encoderRightCps=0;
  uint32_t statusTimestampMs=0;
  char motionState[16]="IDLE";
  uint32_t calibrationGeneration=0; char calibrationResult[24]="NONE", fault[24]="NONE"; };
inline Status status{};
inline Status snapshot() { auto copy=status; copy.connected=mock::connected; copy.safetyEvents=mock::safetyEvents; return copy; }
inline void cancelScene() {}
}  // namespace zumo_link

enum class AudioState { Ready, Playing };
struct AudioStatus { AudioState state; bool cuePlaying, wakeChimePlaying; };
inline struct {
  AudioStatus snapshot() { return {mock::playing ? AudioState::Playing : AudioState::Ready, mock::cuePlaying, mock::chimePlaying}; }
  void cancelOutput() { ++mock::cancelOutputCalls; mock::playing = mock::cuePlaying = mock::chimePlaying = false; }
} audioEngine;
namespace reaction_player {
inline bool active() { return mock::reactionActive; }
inline void cancel() { ++mock::reactionCancelCalls; mock::reactionActive = false; }
}
namespace wake_word {
inline bool conversationReadyReaction() {
  ++mock::readyReactions; mock::cuePlaying = mock::reactionActive = true; return true;
}
}

namespace camera_service {
struct Frame { size_t size; uint32_t timestampMs; uint16_t width, height; };
inline bool available = true;
inline uint32_t capturedAt = 1000;
inline unsigned copies = 0;
inline bool copyLatestJpeg(uint8_t* destination, size_t capacity, Frame* frame) {
  ++copies;
  if (!available || capacity < 4) return false;
  const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
  memcpy(destination, jpeg, sizeof(jpeg)); *frame = {4, capturedAt, 640, 480}; return true;
}
}

namespace robot_actions {
inline const char* name(Command command) { for(const auto& d:definitions)if(d.command==command)return d.toolName;return "Idle"; }
inline Guard::Guard() { ++mock::controlDepth; }
inline Guard::~Guard() {
  assert(mock::controlDepth>0); --mock::controlDepth;
  if (!mock::controlDepth && mock::afterControlRelease) {
    auto hook=mock::afterControlRelease;mock::afterControlRelease=nullptr;hook();
  }
}
inline Result outcome = Result::Running;
inline bool recoveryEligible=false;
inline const char* recoveryFailure="TRACK_MISMATCH";
inline bool lastEdgePermit=false;
inline int actualTravelMm=100;
inline int actualTurn=200;
inline unsigned actualRecoveries=0;
inline const char* recoveryCause="";
inline bool positionKnown=true;
inline bool recoverableMotionFailure(const zumo_link::Status&) { return recoveryEligible; }
inline Status snapshot() { return {Command::Forward,"Actual movement outcome",mock::actionActive,mock::generation,outcome,actualTravelMm,actualTurn,
  outcome==Result::Recoverable ? recoveryFailure : outcome==Result::EdgeRetreated ? "EDGE" : "",
  outcome==Result::EdgeRetreated ? 17 : 0, outcome==Result::EdgeRetreated ? 100 : 0,actualRecoveries,recoveryCause,positionKnown}; }
inline uint32_t generation() { return mock::generation; }
inline const char* unavailableReason(Command) { return mock::actionAvailable ? "" : "Not ready"; }
inline void stop(const char*, bool) {
  assert(mock::criticalDepth == 0 && mock::controlDepth > 0);
  ++mock::stops; ++mock::generation; mock::actionActive = false; outcome = Result::Cancelled;
  robot_session::cancel();
}
inline bool setExpression(const char*, uint32_t) {
  if (mock::beforeStartHook) { auto hook=mock::beforeStartHook; mock::beforeStartHook=nullptr; hook(); }
  return mock::actionAvailable;
}
inline void cancelExpression() { ++mock::expressionCancels; }
inline void suspendForReconnect() { ++mock::stops; mock::actionActive=false; outcome=Result::Failed; positionKnown=false; }
inline bool startFromLive(const ToolRequest& request, uint32_t expectedGeneration) {
 lastEdgePermit=request.edgeRetreat;
 const auto command=request.command;
  assert(mock::criticalDepth == 0);
  assert(mock::controlDepth > 0);
  if (mock::beforeStartHook) mock::beforeStartHook();
  ++mock::actionAttempts;
  if (expectedGeneration != mock::generation || !mock::actionAvailable || !validCommand(static_cast<int>(command)) || mock::actionActive) return false;
  if (command == Command::Stop) { stop("Cloud Stop"); return true; }
  ++mock::dispatched;
  mock::dispatchedCommand = static_cast<int>(command);
  mock::actionActive = true; outcome = Result::Running;
  actualRecoveries=0; recoveryCause=""; positionKnown=true;
  return true;
}
}  // namespace robot_actions

