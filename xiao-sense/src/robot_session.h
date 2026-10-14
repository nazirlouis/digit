#pragma once
#include <cstddef>
#include <cstdint>
#include "camera_frame_limits.h"
namespace robot_session {
struct Status {
  bool ready, listening;
  uint32_t recognized;
  uint32_t samples=0,chunks=0,feedChunks=0,fetchChunks=0,totalFeeds=0,totalFetches=0,droppedFrames=0,liveSentSamples=0;
  uint16_t micPeak=0;
  int lastCommand;
  char error[128];
  int candidateCommand;
  char result[48];
  bool pendingAction, thinking, dryRun;
  bool waitingForReply=false, waitingForVoice=false, toolPending=false;
  uint32_t waitMs=0;
  uint32_t latencyMs;
  char transcript[256], reason[128];
  bool liveActive, liveConnected, liveProbe;
  uint32_t liveVideoFrames, liveVideoBytes, liveVideoCapturedAt;
  uint32_t liveVideoSentAt, sessionStartedAt;
  char currentEmotion[12];
  char liveResponse[512];
  bool liveConnecting;
  uint32_t liveConnectionCount;
  char lastLiveError[128];
  uint32_t liveErrorAt, liveErrorCount;
  char liveConnectionStatus[80];
  uint32_t liveAttemptCount, liveAttemptStarted;
  bool taskActive;
  uint32_t taskMoves;
  char taskGoal[128], taskPhase[64], taskResult[128];
  bool liveTextOnly, liveTextPending;
  uint32_t liveResponseId, lastImageId, lastImageCapturedAt;
  size_t lastImageBytes;
};
// Small control snapshot for real-time tasks; transcripts stay in diagnostics.
struct Activity {
  bool ready, listening, pendingAction;
  bool liveActive, liveConnected, liveProbe, dryRun;
  char result[48];
};
bool liveActive();
bool textSession();
Activity activity();
void begin();
void listen(uint32_t generation);
void submit(const int32_t* stereo,size_t frames);
void audioFault();
// Compatibility query: ER 2 never generates spoken audio.
inline bool liveSpeaking(){return false;}
// testOnly overrides the saved action preference for this one session.
bool startLive(uint32_t generation, bool probe = false, bool testOnly = false);
// Starts an observation session if idle, or steers the current task.
bool sendText(uint32_t generation, const char* text);
inline constexpr size_t kMaxPreviewBytes = camera_frame_limits::kMaxJpegBytes;
// Copies the exact last successfully uploaded JPEG, including after End/Stop.
size_t copyLastImage(uint8_t* output, size_t capacity, uint32_t expectedId);
void cancel();
// Cancel all session work and replace the socket with a fresh idle connection.
// The old cloud conversation/resumption handle is discarded by its worker.
void resetConnection();
// Wi-Fi loss immediately ends media and replaces the background socket worker.
void networkDisconnected();
void poll();
Status snapshot();
}  // namespace robot_session
