#include "action_mission.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "emotion_voice.h"
#include "vision_service.h"
#include "zumo_link.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
using robot_actions::Command;
using Vision = vision_service::Status;
enum class Beat { Idle, Present, Observe, Turn, Drive, Brake, Reveal,
                  EscapeRead, Celebrate, Finish };
constexpr uint32_t kFreshMs = 1200, kSettleMs = 120, kConfirmMs = 1800;
constexpr int kApproachSpeed = 2800, kCloseSpeed = 2400, kHelloStopMm = 100;
constexpr const char* labels[] = {"BATTERY", "MARKER", "SCREWDRIVER"};
Command task = Command::None;
Beat beat = Beat::Idle, afterPresent = Beat::Observe, afterTurn = Beat::Observe, afterBrake = Beat::Observe;
bool running = false, ownsVision = false, objects = false;
bool issued = false, cueIssued = false, cheering = false, centerChirp = false;
bool exitTarget = false, sawTarget = false, welcomed = false, lossReacted = false;
bool closeArrival = false, stopConfirmed = false;
unsigned personHits = 0, objectHits = 0;
uint32_t candidateAt = 0;
int driveSpeed = kApproachSpeed;
const char* message = "Idle";
const char* expression = "CURIOUS";
const char* caption = "SEARCHING";
AudioCue cue = AudioCue::None;
int candidateClass = -1, candidateX = 0, candidateY = 0, candidateArea = 0;
int recognizedClass = -1;
int heading = 0, turnAngle = 0, scans = 0, corrections = 0, advances = 0;
int scanOrigin = 0, scanIndex = 0, bestHeading = 0, bestRange = 0;
uint32_t started = 0, at = 0, freshAfter = 0, lastFrame = 0, lastCapture = 0, lastFreshAt = 0;
uint32_t lastDrive = 0, driveFor = 0, shownAt = 0, limitMs = 0;
uint32_t stationaryAfter = 0, settleUntil = 0, rangeReply = 0;
uint32_t turnLimitMs = 0;
int rangeSamples = 0, ranges[3] = {};

int normalized(int degrees) {
  while (degrees > 180) degrees -= 360;
  while (degrees < -180) degrees += 360;
  return degrees;
}
bool stopped(const zumo_link::Status& z) {
  return !z.left && !z.right && !z.lookActive && !z.manualDriving &&
      static_cast<int32_t>(z.statusTimestampMs - stationaryAfter) > 0 &&
      millis() - z.statusTimestampMs <= 300;
}
void stopVision() {
  if (ownsVision) vision_service::setEnabled(false);
  ownsVision = false;
}
void abort(const char* reason) {
  // Transport failures cannot rely on another display acknowledgement to end.
  message = reason; running = false; beat = Beat::Idle;
  audioEngine.cancelOutput(); stopVision(); zumo_link::stop();
}
void observe() {
  beat = Beat::Observe; at = millis(); freshAfter = at; lastFreshAt = at;
  lastFrame = lastCapture = 0; candidateClass = -1;
  personHits = objectHits = 0; candidateAt = 0;
}
void present(const char* face, const char* label, AudioCue sound, Beat next, const char* phase) {
  expression = face; caption = label; cue = sound; afterPresent = next;
  message = phase; beat = Beat::Present; at = millis(); issued = cueIssued = false;
}
void outcome(const char* phase, bool happy = false, const char* label = "NOT_SURE") {
  zumo_link::stop(); stationaryAfter = millis(); audioEngine.cancelOutput(); stopVision();
  present(happy ? "HAPPY" : "CONFUSED", label, happy ? AudioCue::Happy : AudioCue::Confused, Beat::Finish, phase);
}
void rangeStart() { rangeSamples = 0; rangeReply = 0; at = millis(); }
void enter(Beat next) {
  beat = next; at = millis(); issued = false;
  if (next == Beat::Observe) observe();
  if (next == Beat::EscapeRead) rangeStart();
}
void turn(int angle, Beat next) {
  turnAngle = normalized(angle); afterTurn = next;
  beat = Beat::Turn; issued = false; at = millis();
  if (!turnAngle) enter(next);
}
void brake(Beat next, bool sendStop = true) {
  if (sendStop) zumo_link::stop(true);
  stationaryAfter = millis(); stopConfirmed = false;
  beat = Beat::Brake; afterBrake = next; at = millis();
}
void drive() {
  beat = Beat::Drive; at = millis(); lastDrive = 0;
  const auto z = zumo_link::snapshot();
  const bool close = task == Command::ComeSayHello && z.rangeValid && z.distanceMm < 300;
  driveSpeed = close ? kCloseSpeed : kApproachSpeed;
  driveFor = (driveSpeed * 1000U + zumo_link::acceleration() - 1) / zumo_link::acceleration() + (close ? 80 : 180);
}
void celebrate() {
  stopVision(); zumo_link::stop(); stationaryAfter = millis(); audioEngine.cancelOutput();
  beat = Beat::Celebrate; at = millis(); issued = cheering = centerChirp = false;
  message = task == Command::Escape ? "Found the way out!" : "There you are!";
}
bool freshFrame(const Vision& v, uint32_t now) {
  return v.hasResult && v.imageWidth > 0 && v.imageHeight > 0 &&
      v.resultAgeMs <= kFreshMs && now - v.captureTimestampMs <= kFreshMs;
}
bool goodFrame(const Vision& v, uint32_t now) {
  return freshFrame(v, now) && static_cast<int32_t>(v.captureTimestampMs - freshAfter) >= 0;
}
const Vision::Box* select(const Vision& v) {
  const Vision::Box* best = nullptr;
  int bestRank = -1000000;
  for (unsigned i = 0; i < v.boxCount && i < sizeof(v.boxes)/sizeof(v.boxes[0]); ++i) {
    const auto& box = v.boxes[i];
    if (box.x1 < 0 || box.y1 < 0 || box.x2 > v.imageWidth || box.y2 > v.imageHeight ||
        box.x2 <= box.x1 || box.y2 <= box.y1 || box.scorePercent < (objects ? 50 : vision_service::kPersonConfidencePercent)) continue;
    if (objects ? (box.classId >= 3 || (task == Command::Escape && box.classId != 1)) : box.classId != 0) continue;
    const int area = (box.x2-box.x1) * (box.y2-box.y1);
    const int rank = task == Command::Identify ? -abs((box.x1+box.x2)/2-v.imageWidth/2)*100 + area/100 : area;
    if (rank > bestRank) { bestRank = rank; best = &box; }
  }
  return best;
}
void searchAgain() {
  if (task == Command::Identify) return;
  if (++scans > 12) { outcome(objects ? "I lost the exit marker" : "I could not find you", false, "LOST"); return; }
  const bool lost = sawTarget && !lossReacted;
  lossReacted |= lost;
  present(lost ? "CONFUSED" : "CURIOUS", lost ? "LOST" : "SEARCHING",
          lost ? AudioCue::Confused : AudioCue::None, Beat::Turn, lost ? "Where did you go?" : "Looking around");
  turnAngle = 30; afterTurn = Beat::Observe;
}
void beginEscapeScan() {
  scanOrigin = heading; scanIndex = 0; bestRange = 0; bestHeading = heading;
  // Measure the current heading too: never choose an opening from one old sample.
  present("CURIOUS", "OBSTACLE", AudioCue::Curious, Beat::EscapeRead, "Hmm... checking for a way through");
}
void rangeRead(const zumo_link::Status& z, uint32_t now) {
  const uint32_t stamp = now - z.rangeAgeMs;
  if (now-at >= kSettleMs && z.rangeValid && z.rangeAgeMs < 250 &&
      static_cast<int32_t>(stamp - (at+kSettleMs)) >= 0 && stamp != rangeReply && rangeSamples < 3) {
    rangeReply = stamp; ranges[rangeSamples++] = z.distanceMm;
  }
  if (rangeSamples < 3 && now-at < 1200) return;
  if (rangeSamples == 3) {
    std::sort(ranges, ranges+3); // One far outlier must not invent an opening.
    if (ranges[1] > bestRange) { bestRange = ranges[1]; bestHeading = heading; }
  }
  ++scanIndex;
  if (scanIndex < 4) {
    const int offset = scanIndex == 1 ? 60 : scanIndex == 2 ? -60 : 180;
    turn(normalized(scanOrigin + offset - heading), Beat::EscapeRead);
  } else {
    if (bestRange < 500) { outcome("I need a little help; no clear opening", false, "OBSTACLE"); return; }
    turn(normalized(bestHeading-heading), Beat::Observe);
    message = "That way looks clearer";
  }
}
void inspect(const Vision& v, const zumo_link::Status& z, uint32_t now) {
  if (!stopped(z)) { if (now-at > 2000) abort("Waiting for fresh stopped motor status"); return; }
  if (!goodFrame(v, now) || v.resultTimestampMs == lastFrame || v.captureTimestampMs == lastCapture) {
    if (now-lastFreshAt > 6000) outcome("I cannot see clearly right now");
    return;
  }
  lastFrame = v.resultTimestampMs; lastCapture = v.captureTimestampMs; lastFreshAt = now;
  if (candidateAt && now-candidateAt > kConfirmMs) { objectHits = personHits = 0; candidateClass = -1; }
  if (objects) objectHits = (objectHits << 1) & 15;
  else personHits = (personHits << 1) & 7;
  const auto* box = select(v);
  if (!box) {
    if (!objects) {
      message = candidateAt && now-candidateAt <= kConfirmMs ? "Person briefly lost; checking again" : "Looking for you";
      if ((candidateAt && now-candidateAt <= kConfirmMs) || now-at < 1800) return;
    }
    if (task == Command::Escape && !exitTarget) {
      if (now-at < 700) return;
      if (!z.rangeValid || z.rangeAgeMs >= 250 || z.distanceMm < 500) { beginEscapeScan(); return; }
      if (++advances > 18) { outcome("I need help finding the exit", false, "LOST"); return; }
      present("SERIOUS", "EXIT", AudioCue::None, Beat::Drive, "Carefully exploring");
    } else if (now-at >= (task == Command::Identify ? 7000U : 1000U)) {
      if (task == Command::Identify) outcome("I'm not sure what that is");
      else searchAgain();
    }
    return;
  }
  const int x = (box->x1+box->x2)/2, y = (box->y1+box->y2)/2;
  const int area = (box->x2-box->x1)*(box->y2-box->y1);
  const int category = objects ? box->classId : 0;
  const bool consistent = candidateClass == category && abs(x-candidateX) <= v.imageWidth/6 &&
      abs(y-candidateY) <= v.imageHeight/6 && area <= candidateArea*2 && candidateArea <= area*2;
  if (!consistent) { objectHits = personHits = 0; candidateAt = now; }
  candidateClass = category; candidateX = x; candidateY = y; candidateArea = area;
  if (objects) {
    objectHits |= 1;
    const unsigned hits = (objectHits & 1) + ((objectHits >> 1) & 1) + ((objectHits >> 2) & 1) + ((objectHits >> 3) & 1);
    if (hits < 3) { message = "Let me get a good look"; return; }
  }
  else {
    personHits |= 1;
    const unsigned hits = (personHits & 1) + ((personHits >> 1) & 1) + ((personHits >> 2) & 1);
    if (hits < 2) { message = "Person seen; confirming"; return; }
  }
  sawTarget = true; scans = 0;
  if (task == Command::Identify) {
    recognizedClass = box->classId; stopVision();
    present("SURPRISED", labels[recognizedClass], AudioCue::Surprised, Beat::Reveal, "Oh! I recognize that");
    return;
  }
  if (task == Command::Escape) exitTarget = true;
  const int error = x-v.imageWidth/2;
  if (abs(error) > v.imageWidth/10) {
    if (++corrections > 18) { outcome("I cannot get a clear view", false, "LOST"); return; }
    int degrees = std::max(-18, std::min(18, error*60/v.imageWidth));
    if (abs(degrees) < 6) degrees = degrees < 0 ? -6 : 6;
    turn(degrees, Beat::Observe); message = "Turning toward the target"; return;
  }
  corrections = 0;
  if (!z.rangeValid || z.rangeAgeMs >= 250) {
    message = "Waiting for a clear distance reading";
    if (now-at > 4500) outcome("I cannot measure the path ahead", false, "OBSTACLE");
    return;
  }
  const int stopAt = task == Command::ComeSayHello ? kHelloStopMm : 550;
  if (closeArrival && z.distanceMm > stopAt+20) closeArrival = false;
  if (z.distanceMm <= stopAt || closeArrival) { celebrate(); return; }
  if (!welcomed) {
    welcomed = true;
    present("HAPPY", objects ? "EXIT" : "PERSON", AudioCue::Happy, Beat::Observe,
            objects ? "I see the way out!" : "There you are! Coming to say hello");
    return;
  }
  present(objects ? "SERIOUS" : "HAPPY", objects ? "EXIT" : "PERSON", AudioCue::None,
          Beat::Drive, objects ? "Heading for the exit" : "Coming to say hello");
}
}

namespace action_mission {
bool start(Command value) {
  if (value != Command::ComeSayHello && value != Command::Identify && value != Command::Escape) return false;
  cancel(); task = value; running = true;
  started = at = stationaryAfter = millis(); scans = corrections = advances = 0; heading = 0;
  exitTarget = sawTarget = welcomed = lossReacted = closeArrival = false; recognizedClass = -1;
  objects = value != Command::ComeSayHello;
  limitMs = value == Command::Identify ? 12000 : 150000;
  ownsVision = objects ? vision_service::setBatteryEnabled(true) : vision_service::setEnabled(true);
  if (!ownsVision) { abort("Detection model unavailable"); return false; }
  present("CURIOUS", value == Command::Escape ? "EXIT" : "SEARCHING", AudioCue::Curious,
          Beat::Observe, value == Command::Identify ? "Let me see!" : value == Command::Escape ? "Let's explore" : "Looking for you");
  return true;
}
void cancel() { running = false; beat = Beat::Idle; stopVision(); }
bool active() { return running; }
const char* phase() { return message; }
void poll() {
  if (!running) return;
  const uint32_t now = millis();
  const auto z = zumo_link::snapshot();
  const auto v = vision_service::snapshot();
  if (ownsVision && (!v.enabled || v.batteryMode != objects)) { outcome("Detection was changed; action stopped"); return; }
  if (now-started > limitMs && beat != Beat::Finish && !(beat == Beat::Present && afterPresent == Beat::Finish)) {
    outcome("I need a little help; search time is up", false, "LOST"); return;
  }
  switch (beat) {
    case Beat::Present:
      if (!issued) {
        if (stopped(z) && zumo_link::scene(expression, caption)) { issued = true; at = now; }
        else if (now-at > 2000) abort("Display or stopped motor status unavailable");
        return;
      }
      if (!cueIssued) {
        if (!strcmp(z.sceneResult,"SENDING")) { if (now-at > 1500) abort("Display acknowledgement timed out"); return; }
        if (strcmp(z.sceneResult,"APPLIED")) { abort("Display update failed"); return; }
        if (cue != AudioCue::None && !audioEngine.playCue(cue)) { abort("Voice unavailable"); return; }
        cueIssued = true; shownAt = now;
      }
      // Let expressive beats be seen and heard, without delaying each drive step.
      if (now-shownAt < (afterPresent == Beat::Drive ? 180U : afterPresent == Beat::Finish ? 1200U : 450U)) return;
      if (cue != AudioCue::None && audioEngine.cuePlaying()) {
        if (now-shownAt <= emotionDurationMs(cue) + 500) return;
        abort("Voice playback timed out"); return;
      }
      enter(afterPresent);
      if (beat == Beat::Drive) drive();
      break;
    case Beat::Observe: inspect(v,z,now); break;
    case Beat::Turn:
      if (!issued) {
        if (stopped(z) && zumo_link::look(turnAngle,z.controlGeneration)) {
          issued = true; at = now;
          // Snapshot the selected ramp, matching the UART turn watchdog with margin.
          const auto acceleration = zumo_link::acceleration();
          turnLimitMs = 7000U + 2U * ((2400000U + acceleration - 1) / acceleration);
        }
        else if (now-at > 2000) outcome("Turn could not start");
        return;
      }
      if (now-at > turnLimitMs) { abort("Turn timed out"); return; }
      if (z.lookActive) return;
      if (strcmp(z.lookResult,"DONE")) { outcome("Turn was interrupted"); return; }
      heading = normalized(heading+turnAngle);
      brake(afterTurn,false);
      break;
    case Beat::Drive: {
      const bool course = task == Command::Escape && !exitTarget;
      const bool frameFresh = goodFrame(v,now);
      const auto* box = frameFresh ? select(v) : nullptr;
      const bool aligned = box && abs((box->x1+box->x2)/2-v.imageWidth/2) <= v.imageWidth/8;
      const int stopAt = course ? 350 : task == Command::ComeSayHello ? kHelloStopMm : 550;
      const bool closeApproach = task == Command::ComeSayHello;
      if (closeApproach && z.approachArrived) closeArrival = true;
      if (!z.statusTimestampMs || now-z.statusTimestampMs > 300) { abort("Motor status became stale"); return; }
      if (!z.rangeValid || z.rangeAgeMs >= 250 || z.distanceMm <= stopAt || closeArrival ||
          !frameFresh || (!course && !aligned) || now-at >= driveFor) { brake(Beat::Observe); return; }
      if (!lastDrive || now-lastDrive >= 80) {
        if (!zumo_link::safeDrive(driveSpeed,driveSpeed,closeApproach ? stopAt : 150)) {
          outcome("Approach was rejected",false,"OBSTACLE"); return;
        }
        lastDrive = now;
      }
      break;
    }
    case Beat::Brake:
      if (now-at > 2000) { abort("Motors did not confirm stop"); return; }
      if (!stopped(z)) { stopConfirmed = false; return; }
      if (!stopConfirmed) { stopConfirmed = true; settleUntil = now+kSettleMs; }
      if (static_cast<int32_t>(now-settleUntil) < 0) return;
      enter(afterBrake);
      break;
    case Beat::Reveal:
      outcome(recognizedClass == 0 ? "That is a battery!" : recognizedClass == 1 ? "That is a marker!" : "That is a screwdriver!", true, labels[recognizedClass]);
      break;
    case Beat::EscapeRead:
      if (!stopped(z)) { if (now-at > 2000) abort("Motor status unavailable during scan"); return; }
      rangeRead(z,now); break;
    case Beat::Celebrate:
      if (!issued) {
        if (stopped(z) && zumo_link::wiggle(z.controlGeneration,true)) { issued = true; at = now; }
        else if (now-at > 2500) outcome("Found the target",true,"FOUND");
        return;
      }
      if (now-at > 5500) { abort("Celebration timed out"); return; }
      if (z.lookActive) {
        if (!cheering && !strcmp(z.lookResult,"WIGGLING")) {
          if (!audioEngine.playCue(AudioCue::Excited,VoiceTiming::Gesture)) { abort("Celebration voice unavailable"); return; }
          cheering = true;
        }
        if (!centerChirp && !strcmp(z.lookResult,"CENTERING")) {
          if (!audioEngine.playCue(AudioCue::Neutral,VoiceTiming::Gesture)) { abort("Celebration voice unavailable"); return; }
          centerChirp = cheering = true;
        }
        return;
      }
      audioEngine.cancelOutput();
      if (strcmp(z.lookResult,"DONE")) { outcome("Celebration interrupted"); return; }
      stationaryAfter = now;
      present(task == Command::Escape ? "EXCITED" : "LOVE", task == Command::Escape ? "EXIT" : "FOUND",
              task == Command::Escape ? AudioCue::Excited : AudioCue::Love, Beat::Finish, message);
      break;
    case Beat::Finish:
      stopVision(); audioEngine.cancelOutput(); running = false; beat = Beat::Idle;
      break;
    default: break;
  }
}
}
