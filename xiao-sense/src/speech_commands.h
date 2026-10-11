#pragma once
#include <cstddef>
#include <cstdint>
namespace speech_commands {
struct Status {
  bool ready, listening;
  uint32_t recognized;
  int lastCommand;
  float score;
  char error[128];
  uint32_t samples, chunks, inferenceUs;
  uint16_t micPeak;
  int candidateCommand;
  float candidateScore;
  char result[48];
  uint32_t feedChunks, fetchChunks, totalFeeds, totalFetches, droppedFrames;
  uint16_t modelPeak;
  char decoderText[128];
  bool pendingAction, thinking, dryRun;
  uint32_t latencyMs;
  char transcript[256], reason[128];
  bool liveActive, liveConnected, liveSpeaking, liveProbe;
  uint32_t liveSentSamples, liveReceivedSamples, livePlayedSamples;
  uint32_t liveVideoFrames, liveVideoBytes, liveVideoCapturedAt, idleRemainingMs;
  char liveResponse[512];
  bool liveConnecting;
  uint32_t liveConnectionCount;
  bool taskActive;
  uint32_t taskMoves;
  char taskGoal[128], taskPhase[64], taskResult[128];
};
// Small control snapshot for real-time tasks; transcripts stay in diagnostics.
struct Activity {
  bool ready, listening, pendingAction;
  bool liveActive, liveConnected, liveSpeaking, liveProbe, dryRun;
  char result[48];
};
bool liveActive();
bool liveSpeaking();
Activity activity();
void begin();
void listen(uint32_t generation);
// testOnly overrides the saved action preference for this one session.
bool startLive(uint32_t generation, bool probe = false, bool testOnly = false);
void submit(const int32_t* stereo, size_t frames);
size_t renderAudio(int16_t* samples, size_t count);
void cancel();
void audioFault();
// Wi-Fi loss immediately ends media and replaces the background socket worker.
void networkDisconnected();
void poll();
Status snapshot();
}  // namespace speech_commands
