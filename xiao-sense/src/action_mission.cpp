#include "action_mission.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "emotion_voice.h"
#include "reaction_player.h"
#include "vision_service.h"
#include "zumo_link.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
using robot_actions::Command;
using Vision = vision_service::Status;
enum class Beat { Idle, Present, Observe, Turn, Drive, Brake, Reveal,
                  EscapeRead, Finish };
constexpr uint32_t kFreshMs = 1200, kSettleMs = 120, kConfirmMs = 1800;
constexpr int kApproachSpeed = 3000, kCloseSpeed = 3000, kHelloStopMm = 100;
constexpr int kExitBatteryStopMm = 100;
constexpr int kMazeClearanceMm = 50;
// Match the local stopped-arrival tolerance: a heading the Zumo considers
// already at its wall cannot be offered as a new opening to drive into.
constexpr int kMazeOpeningMm = kMazeClearanceMm + 10;
constexpr uint8_t kExitBatteryClass = 0;
constexpr const char* labels[] = {"BATTERY", "MARKER", "SCREWDRIVER"};
Command task = Command::None;
Beat beat = Beat::Idle, afterPresent = Beat::Observe, afterTurn = Beat::Observe, afterBrake = Beat::Observe;
bool running = false, ownsVision = false, objects = false;
bool issued = false;
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
int validScanHeadings = 0;
uint32_t started = 0, at = 0, freshAfter = 0, lastFrame = 0, lastCapture = 0, lastFreshAt = 0;
uint32_t lastDrive = 0, limitMs = 0;
uint32_t stationaryAfter = 0, settleUntil = 0, rangeReply = 0;
uint32_t turnLimitMs = 0;
uint32_t lastFreshRangeMs = 0;
uint32_t lastFreshStoppedMs = 0;
uint32_t activeJob = 0;
uint32_t steeringFrame = 0;
uint32_t reactionToken = 0;

int normalized(int degrees) {
  while (degrees > 180) degrees -= 360;
  while (degrees < -180) degrees += 360;
  return degrees;
}
bool stopped(const zumo_link::Status& z) {
  return !z.left && !z.right && !z.lookActive && !z.manualDriving && !z.motionActive &&
      static_cast<int32_t>(z.statusTimestampMs - stationaryAfter) > 0 &&
      millis() - z.statusTimestampMs <= 1000;
}
void stopVision() {
  if (ownsVision) vision_service::setEnabled(false);
  ownsVision = false;
}
void abort(const char* reason) {
  // Transport failures cannot rely on another display acknowledgement to end.
  message = reason; running = false; beat = Beat::Idle;
  reaction_player::cancel(); stopVision(); zumo_link::stop();
}
void observe() {
  beat = Beat::Observe; at = millis(); freshAfter = at; lastFreshAt = at;
  lastFrame = lastCapture = 0; candidateClass = -1;
  personHits = objectHits = 0; candidateAt = 0;
}
void present(const char* face, const char* label, AudioCue sound, Beat next, const char* phase) {
  expression = face; caption = label; cue = sound; afterPresent = next;
  message = phase; beat = Beat::Present; at = millis(); issued = false; reactionToken=0;
}
void outcome(const char* phase, bool happy = false, const char* label = "NOT_SURE") {
  zumo_link::stop(); stationaryAfter = millis(); reaction_player::cancel(); stopVision();
  present(happy ? "HAPPY" : "CONFUSED", label, happy ? AudioCue::Happy : AudioCue::Confused, Beat::Finish, phase);
}
void rangeStart() { activeJob = 0; rangeReply = 0; at = millis(); }
void enter(Beat next) {
  beat = next; at = millis(); issued = false;
  if (next == Beat::Observe) observe();
  if (next == Beat::EscapeRead) rangeStart();
}
void turn(int angle, Beat next) {
  turnAngle = normalized(angle); afterTurn = next;
  beat = Beat::Turn; issued = false; activeJob=0; at = millis();
  if (!turnAngle) enter(next);
}
void brake(Beat next, bool sendStop = true) {
  if (sendStop) zumo_link::cancelMotion(true);
  activeJob=0;
  stationaryAfter = millis(); stopConfirmed = false;
  beat = Beat::Brake; afterBrake = next; at = millis();
}
void drive() {
  beat = Beat::Drive; at = millis(); lastDrive = 0; activeJob=0; steeringFrame=0;
  const auto z = zumo_link::snapshot();
  const bool close = (task == Command::Escape && !exitTarget) ||
      ((task == Command::ComeSayHello || task == Command::Escape) && z.rangeValid && z.distanceMm < 300);
  driveSpeed = close ? kCloseSpeed : kApproachSpeed;
}
void celebrate() {
  stopVision(); zumo_link::stop(); stationaryAfter = millis(); reaction_player::cancel();
  message = task == Command::Escape ? "Found the way out!" : "There you are!";
  // Arrival personality is face and voice; no surprise motor gesture in a maze.
  present(task==Command::Escape ? "EXCITED" : "LOVE", task==Command::Escape ? "EXIT" : "FOUND",
          task==Command::Escape ? AudioCue::Excited : AudioCue::Love, Beat::Finish, message);
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
    if (objects ? (box.classId >= 3 || (task == Command::Escape && box.classId != kExitBatteryClass)) : box.classId != 0) continue;
    const int area = (box.x2-box.x1) * (box.y2-box.y1);
    const int rank = task == Command::Identify ? -abs((box.x1+box.x2)/2-v.imageWidth/2)*100 + area/100 : area;
    if (rank > bestRank) { bestRank = rank; best = &box; }
  }
  return best;
}
void searchAgain() {
  if (task == Command::Identify) return;
  if (++scans > 12) { outcome(objects ? "I lost the battery" : "I could not find you", false, "LOST"); return; }
  const bool lost = sawTarget && !lossReacted;
  lossReacted |= lost;
  present(lost ? "CONFUSED" : "CURIOUS", lost ? "LOST" : "SEARCHING",
          lost ? AudioCue::Confused : AudioCue::None, Beat::Turn, lost ? "Where did you go?" : "Looking around");
  turnAngle = 30; afterTurn = Beat::Observe;
}
void beginEscapeScan() {
  scanOrigin = heading; scanIndex = 0; bestRange = 0; bestHeading = heading;
  validScanHeadings = 0;
  // Measure the current heading too: never choose an opening from one old sample.
  present("CURIOUS", "SEARCHING", AudioCue::Curious, Beat::EscapeRead, "Hmm... checking for a way through");
}
void rangeRead(const zumo_link::Status& z, uint32_t now) {
  if (!activeJob) {
    if (zumo_link::armMotion()) activeJob=zumo_link::measureJob();
    if (!activeJob && now-at > 2500) abort("Settled distance measurement could not start");
    return;
  }
  if (now-at > 5000) { abort("Settled distance measurement timed out"); return; }
  if (z.motionJob != activeJob || strcmp(z.motionState,"DONE")) return;
  if (strcmp(z.motionResult,"MEASURED") || z.motionDistanceMm < 0 || z.motionRangeAgeMs > 1000) {
    abort("Distance measurement did not complete"); return;
  }
  zumo_link::ackMotion(activeJob); activeJob=0;
  // The local job supplies three *new* settled readings at its reported
  // heading. Delayed status packets from another heading cannot count.
  heading=normalized(z.motionHeadingDeciDeg/10);
  if (scanIndex==0) scanOrigin=heading;
  ++validScanHeadings;
  if (z.motionDistanceMm > bestRange) { bestRange=z.motionDistanceMm; bestHeading=heading; }
  ++scanIndex;
  if (scanIndex < 3) {
    // Compare front/left/right relative to arrival heading; never sample behind.
    const int offset = scanIndex == 1 ? 90 : -90;
    turn(normalized(scanOrigin + offset - heading), Beat::EscapeRead);
  } else {
    if (bestRange <= kMazeOpeningMm) {
      if (validScanHeadings < 3) outcome("Distance readings incomplete; please check the sensor", false, "NOT_SURE");
      else outcome("I need a little help; no clear opening", false, "OBSTACLE");
      return;
    }
    turn(normalized(bestHeading-heading), Beat::Observe);
    message = "That way looks clearer";
  }
}
void inspect(const Vision& v, const zumo_link::Status& z, uint32_t now) {
  if (!stopped(z)) {
    // Time spent inspecting camera frames is not a missing-controller outage.
    // A single late status after a long Observe phase must not end the action.
    if (now-lastFreshStoppedMs > 2000) abort("No fresh stopped motor status for 2 seconds");
    return;
  }
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
      // Range acceptance and emergency braking belong to the Zumo. The
      // dashboard's transport age must not become a second motion watchdog.
      if (z.rangeValid && z.rangeAgeMs < 1000 && z.distanceMm <= kMazeClearanceMm) { beginEscapeScan(); return; }
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
  if (closeArrival) { celebrate(); return; }
  if (!welcomed) {
    welcomed = true;
    present("HAPPY", objects ? "BATTERY" : "PERSON", AudioCue::Happy, Beat::Observe,
            objects ? "A battery! That must be the way out!" : "There you are! Coming to say hello");
    return;
  }
  present(objects ? "SERIOUS" : "HAPPY", objects ? "BATTERY" : "PERSON", AudioCue::None,
          Beat::Drive, objects ? "Heading for the battery" : "Coming to say hello");
}
}

namespace action_mission {
bool start(Command value) {
  if (value != Command::ComeSayHello && value != Command::Identify && value != Command::Escape) return false;
  cancel(); task = value; running = true;
  activeJob=0; steeringFrame=0;
  started = at = stationaryAfter = millis(); scans = corrections = advances = 0; heading = 0;
  lastFreshRangeMs = started;
  lastFreshStoppedMs = started;
  exitTarget = sawTarget = welcomed = lossReacted = closeArrival = false; recognizedClass = -1;
  objects = value != Command::ComeSayHello;
  limitMs = value == Command::Identify ? 12000 : 150000;
  ownsVision = objects ? vision_service::setBatteryEnabled(true) : vision_service::setEnabled(true);
  if (!ownsVision) { abort("Detection model unavailable"); return false; }
  present("CURIOUS", value == Command::Escape ? "EXIT" : "SEARCHING", AudioCue::Curious,
          Beat::Observe, value == Command::Identify ? "Let me see!" : value == Command::Escape ? "Let's explore" : "Looking for you");
  return true;
}
void cancel() {
  reaction_player::cancel();
  if (running && task != Command::Identify) zumo_link::cancelMotion();
  running = false; beat = Beat::Idle; activeJob=0; stopVision();
}
bool active() { return running; }
const char* phase() { return message; }
void poll() {
  if (!running) return;
  const uint32_t now = millis();
  if (task != Command::Identify) zumo_link::superviseMotion();
  const auto z = zumo_link::snapshot();
  const auto v = vision_service::snapshot();
  if (z.connected && z.rangeValid && z.rangeAgeMs < 250 && z.distanceMm > 0) lastFreshRangeMs = now;
  if (stopped(z)) lastFreshStoppedMs = now;
  if (ownsVision && (!v.enabled || v.batteryMode != objects)) { outcome("Detection was changed; action stopped"); return; }
  if (now-started > limitMs && beat != Beat::Finish && !(beat == Beat::Present && afterPresent == Beat::Finish)) {
    outcome("I need a little help; search time is up", false, "LOST"); return;
  }
  switch (beat) {
    case Beat::Present: {
      if (!issued) {
        if (!stopped(z)) {
          if (now-at>2000) abort("Stopped motor status unavailable");
          return;
        }
        const uint32_t cooldown=cue==AudioCue::Curious && task==Command::Escape && !strcmp(caption,"SEARCHING") ? 8000U : 0U;
        reactionToken=reaction_player::play(expression,caption,cue,reaction_player::Source::Action,cooldown);
        issued=true; at=now;
      }
      const auto reaction=reaction_player::snapshot();
      // Driving never waits for decoration. The face renderer and coordinator
      // continue independently while the Zumo owns the movement job.
      if (afterPresent==Beat::Drive) { enter(afterPresent); drive(); return; }
      const bool done=reaction_player::finished(reactionToken);
      if (!done && !reaction.visible) return;
      if (!done && (task==Command::Identify || afterPresent==Beat::Finish)) return;
      if (!done && now-reaction.visibleAt<180) return;
      // Missing optional audio/display is a skipped reaction, not a failed
      // navigation mission or a reason to alter motor safety.
      enter(afterPresent);
      break;
    }
    case Beat::Observe: inspect(v,z,now); break;
    case Beat::Turn:
      if (!issued) {
        if (stopped(z) && zumo_link::armMotion()) {
          const auto acceleration = zumo_link::acceleration();
          turnLimitMs = 7000U + 2U * ((3000000U + acceleration - 1) / acceleration);
          activeJob=zumo_link::turnJob(turnAngle,turnLimitMs);
          if (activeJob) { issued = true; at = now; }
        }
        else if (now-at > 2000) outcome("Turn could not start");
        return;
      }
      if (now-at > turnLimitMs) { abort("Turn timed out"); return; }
      if (z.motionJob!=activeJob || strcmp(z.motionState,"DONE")) return;
      if (strcmp(z.motionResult,"TURN_DONE")) { outcome("Turn was interrupted"); return; }
      zumo_link::ackMotion(activeJob);
      heading = normalized(z.motionHeadingDeciDeg/10);
      brake(afterTurn,false);
      break;
    case Beat::Drive: {
      const bool course = task == Command::Escape && !exitTarget;
      const bool frameFresh = goodFrame(v,now);
      const auto* box = frameFresh ? select(v) : nullptr;
      const int targetError=box ? (box->x1+box->x2)/2-v.imageWidth/2 : 0;
      const bool aligned = box && abs(targetError) <= v.imageWidth/5;
      const int stopAt = course ? kMazeClearanceMm : task == Command::ComeSayHello ? kHelloStopMm : kExitBatteryStopMm;
      if (activeJob && z.motionJob==activeJob && !strcmp(z.motionState,"DONE")) {
        if (strcmp(z.motionResult,"WALL_STOPPED")) { abort("Drive ended without a wall stop"); return; }
        zumo_link::ackMotion(activeJob); activeJob=0;
        heading=normalized(z.motionHeadingDeciDeg/10);
        if (course) beginEscapeScan();
        else { closeArrival=true; celebrate(); }
        return;
      }
      // Vision can request a change of intent, but stale host ToF/status cannot
      // interrupt local safety-controlled travel. A blind target pursuit stops.
      if ((course && box) || (!course && (!frameFresh || !aligned))) { brake(Beat::Observe); return; }
      if (frameFresh) lastFreshAt=now;
      if (course && now-lastFreshAt > 6000) { brake(Beat::Observe); return; }
      if (!activeJob) {
        if (zumo_link::armMotion()) activeJob=zumo_link::driveJob(driveSpeed,driveSpeed,stopAt,60000);
        if (!activeJob && now-at > 2500) abort("Autonomous drive could not start");
        return;
      }
      if (!course && v.captureTimestampMs!=steeringFrame && now-lastDrive >= 100) {
        // Small visual corrections update this same job; no stop/start cycle.
        const int correction=abs(targetError)<=v.imageWidth/32 ? 0 : std::min(200,abs(targetError)*1000/v.imageWidth);
        if (zumo_link::updateDriveJob(activeJob,driveSpeed+(targetError<0?correction:0),
                                    driveSpeed+(targetError>0?correction:0))) {
          steeringFrame=v.captureTimestampMs; lastDrive=now;
        }
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
      if (!activeJob && !stopped(z)) { if (now-lastFreshStoppedMs > 2000) abort("No fresh stopped motor status during scan for 2 seconds"); return; }
      rangeRead(z,now); break;
    case Beat::Finish:
      stopVision(); reaction_player::cancel(); running = false; beat = Beat::Idle;
      break;
    default: break;
  }
}
}
