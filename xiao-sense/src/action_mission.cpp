#include "action_mission.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "vision_service.h"
#include "zumo_link.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
using robot_actions::Command;
using Vision = vision_service::Status;
enum class Beat { Idle, Present, Observe, Turn, Drive, Brake, Return,
                  EscapeRead, Celebrate, Finish };
constexpr uint32_t kFreshMs = 1200;
constexpr int kApproachSpeed = 2800;
constexpr int kCloseSpeed = 2400;
constexpr int kFindMeStopMm = 100;
constexpr int kChaseStopMm = 50;
constexpr int kChaseResumeMm = 100;
constexpr uint32_t kPersonConfirmMs = 1600;
constexpr const char* labels[] = {"BATTERY", "MARKER", "SCREWDRIVER"};
Command task = Command::None;
Beat beat = Beat::Idle, afterPresent = Beat::Observe, afterTurn = Beat::Observe, afterBrake = Beat::Observe;
bool running = false, ownsVision = false, objects = false, returning = false;
bool issued = false, cueIssued = false, cheering = false, centerChirp = false, chaseNear = false;
bool exitTarget = false, sawTarget = false;
bool closeArrival = false;
unsigned personHits = 0;
uint32_t candidateAt = 0;
int driveSpeed = kApproachSpeed;
const char* message = "Idle";
const char* expression = "CURIOUS";
const char* caption = "SEARCHING";
AudioCue cue = AudioCue::None;
int targetClass = -1, candidateClass = -1, candidateX = 0, matches = 0;
int heading = 0, turnAngle = 0, scans = 0, corrections = 0, advances = 0;
int scanOrigin = 0, scanIndex = 0, bestHeading = 0, bestRange = 0;
uint32_t started = 0, at = 0, freshAfter = 0, lastFrame = 0, lastFreshAt = 0;
uint32_t lastDrive = 0, driveFor = 0, shownAt = 0, limitMs = 0;
uint32_t rangeReply = 0;
int rangeSamples = 0, rangeTotal = 0;

int normalized(int degrees) {
  while (degrees > 180) degrees -= 360;
  while (degrees < -180) degrees += 360;
  return degrees;
}
bool stopped(const zumo_link::Status& z) { return !z.left && !z.right && !z.lookActive && !z.manualDriving; }
void stopVision() {
  if (ownsVision) vision_service::setEnabled(false);
  ownsVision = false;
}
bool visionMode(bool useObjects) {
  objects = useObjects;
  const bool ok = useObjects ? vision_service::setBatteryEnabled(true) : vision_service::setEnabled(true);
  ownsVision = ok;
  return ok;
}
void observe() {
  beat = Beat::Observe; at = millis(); freshAfter = lastFreshAt = at;
  lastFrame = 0; matches = 0; candidateClass = -1;
  personHits = 0; candidateAt = 0;
}
void present(const char* face, const char* label, AudioCue sound, Beat next, const char* phase) {
  expression = face; caption = label; cue = sound; afterPresent = next;
  message = phase; beat = Beat::Present; at = millis(); issued = cueIssued = false;
}
void outcome(const char* phase, bool happy = false, const char* label = "NOT_SURE") {
  zumo_link::stop(); audioEngine.cancelOutput(); stopVision();
  present(happy ? "HAPPY" : "CONFUSED", label, happy ? AudioCue::Happy : AudioCue::Confused, Beat::Finish, phase);
}
void turn(int angle, Beat next) {
  turnAngle = normalized(angle); afterTurn = next;
  beat = Beat::Turn; issued = false; at = millis();
  if (!turnAngle) {
    beat = next;
    if (next == Beat::Observe) observe();
  }
}
void brake(Beat next) {
  zumo_link::stop(true);
  beat = Beat::Brake; afterBrake = next; at = millis();
}
void drive() {
  beat = Beat::Drive; at = millis(); lastDrive = 0;
  // Let the selected ramp reach usable torque, then take a short forward step.
  const auto z = zumo_link::snapshot();
  const bool close = (task == Command::FindMe || task == Command::Chase) && z.rangeValid && z.distanceMm < 300;
  driveSpeed = close ? kCloseSpeed : kApproachSpeed;
  driveFor = (driveSpeed * 1000U + zumo_link::acceleration() - 1) / zumo_link::acceleration() + (close ? 80 : 180);
}
void celebrate() {
  stopVision(); zumo_link::stop(); audioEngine.cancelOutput();
  beat = Beat::Celebrate; at = millis(); issued = cheering = centerChirp = false;
  message = returning ? "Found it and found you!" : task == Command::Escape ? "Found the way out!" : "Found you!";
}
bool freshFrame(const Vision& v, uint32_t now) {
  return v.hasResult && v.resultAgeMs <= kFreshMs && now - v.captureTimestampMs <= kFreshMs;
}
bool goodFrame(const Vision& v, uint32_t now) {
  return freshFrame(v, now) &&
      static_cast<int32_t>(v.captureTimestampMs - freshAfter) >= 0;
}
const Vision::Box* select(const Vision& v) {
  const Vision::Box* best = nullptr;
  int bestRank = -1000000;
  for (unsigned i = 0; i < v.boxCount; ++i) {
    const auto& box = v.boxes[i];
    if (box.x2 <= box.x1 || box.y2 <= box.y1 || box.scorePercent < (objects ? 50 : vision_service::kPersonConfidencePercent)) continue;
    if (objects && (box.classId >= 3 || (targetClass >= 0 && box.classId != targetClass))) continue;
    const int area = (box.x2-box.x1) * (box.y2-box.y1);
    const int rank = (task == Command::Identify || (task == Command::Chase && targetClass < 0))
        ? -abs((box.x1+box.x2)/2-v.imageWidth/2)*100 + area/100 : area;
    if (rank > bestRank) { bestRank = rank; best = &box; }
  }
  return best;
}
void searchAgain() {
  if (task == Command::Identify) return;
  const int maxScans = task == Command::Chase ? 4 : 12;
  if (++scans > maxScans) { outcome(objects ? "Could not find the object" : "Could not find a person", false, "LOST"); return; }
  // One bounded sweep; a lost chase target gets a smaller local sweep.
  const int angle = task == Command::Chase ? (scans == 1 ? -30 : scans == 4 ? -30 : 30) : 30;
  const bool lost = sawTarget && scans == 1;
  present(lost ? "CONFUSED" : "CURIOUS", lost ? "LOST" : "SEARCHING",
          lost ? AudioCue::Confused : AudioCue::None, Beat::Turn, "Looking around");
  turnAngle = angle; afterTurn = Beat::Observe;
}
void beginEscapeScan(const zumo_link::Status& z) {
  scanOrigin = heading; scanIndex = 0;
  bestRange = z.rangeValid && z.rangeAgeMs < 250 ? z.distanceMm : 0; bestHeading = heading;
  turn(60, Beat::EscapeRead);
  message = "Checking openings";
}
void rangeRead(const zumo_link::Status& z, uint32_t now) {
  if (now - at < 250) return;
  const uint32_t stamp = now - z.rangeAgeMs;
  if (z.rangeValid && z.rangeAgeMs < 250 && stamp != rangeReply) {
    rangeReply = stamp; rangeTotal += z.distanceMm; ++rangeSamples;
  }
  if (rangeSamples < 3 && now - at < 900) return;
  if (rangeSamples == 3 && rangeTotal / 3 > bestRange) {
    bestRange = rangeTotal / 3; bestHeading = heading;
  }
  ++scanIndex;
  if (scanIndex < 3) {
    turn(normalized(scanOrigin + (scanIndex == 1 ? -60 : 180) - heading), Beat::EscapeRead);
  } else {
    if (bestRange < 500) { outcome("No measured opening; stopped", false, "OBSTACLE"); return; }
    turn(normalized(bestHeading-heading), Beat::Observe);
    message = "Opening found";
  }
}
void inspect(const Vision& v, const zumo_link::Status& z, uint32_t now) {
  if (!goodFrame(v, now) || v.resultTimestampMs == lastFrame) {
    if (!objects && !goodFrame(v, now)) message = "Waiting for a fresh person image";
    if (now - lastFreshAt > 6000) outcome("Waiting for fresh camera images", false, "NOT_SURE");
    return;
  }
  lastFrame = v.resultTimestampMs;
  lastFreshAt = now;
  if (!objects) {
    personHits = (personHits << 1) & 7;
    if (candidateAt && now-candidateAt > kPersonConfirmMs) { personHits = 0; candidateClass = -1; }
  }
  const auto* box = select(v);
  if (!box) {
    matches = 0;
    if (objects) candidateClass = -1;
    else {
      message = candidateAt && now-candidateAt < kPersonConfirmMs ?
          "Person briefly lost; checking again" : "Looking for a person";
      if ((candidateAt && now-candidateAt < kPersonConfirmMs) || now-at < 1800) return;
    }
    if (task == Command::Escape && !exitTarget) {
      if (now - at < 700) return;
      if (!z.rangeValid || z.rangeAgeMs >= 250 || z.distanceMm < 500) { beginEscapeScan(z); return; }
      if (++advances > 18) { outcome("Course limit reached; no exit marker", false, "LOST"); return; }
      present("SERIOUS", "EXIT", AudioCue::None, Beat::Drive, "Moving through the opening");
    } else if (now - at >= (task == Command::Identify ? 7000U : 1000U)) {
      if (task == Command::Identify) outcome("Not sure what that is", false, "NOT_SURE");
      else searchAgain();
    }
    return;
  }
  const int x = (box->x1 + box->x2) / 2;
  const int category = objects ? box->classId : 0;
  const bool consistent = candidateClass == category && abs(x-candidateX) <= v.imageWidth/4;
  if (consistent) ++matches;
  else { matches = 1; personHits = 0; }
  candidateClass = category; candidateX = x;
  if (objects) { if (matches < 3) return; }
  else {
    candidateAt = now; personHits |= 1;
    const unsigned hits = (personHits & 1) + ((personHits >> 1) & 1) + ((personHits >> 2) & 1);
    if (hits < 2) { message = "Person seen; confirming"; return; }
  }
  sawTarget = true;
  scans = 0;
  if (task == Command::Identify) {
    outcome(box->classId == 0 ? "That is a battery" : box->classId == 1 ? "That is a marker" : "That is a screwdriver", true, labels[box->classId]);
    return;
  }
  if (task == Command::Chase && targetClass < 0) targetClass = box->classId;
  if (task == Command::Escape) exitTarget = true;
  if (!returning && task >= Command::FindBattery && task <= Command::FindScrewdriver) {
    present("SURPRISED", labels[targetClass], AudioCue::Surprised, Beat::Return, "Found the object; coming back to you");
    return;
  }
  const int error = x - v.imageWidth/2;
  if (abs(error) > v.imageWidth/10) {
    if (++corrections > 18) { outcome("Could not center the target", false, "LOST"); return; }
    // Match the physically verified mirrored camera steering in person_follow.
    int degrees = std::max(-18, std::min(18, error * 60 / int(v.imageWidth)));
    if (abs(degrees) < 6) degrees = degrees < 0 ? -6 : 6;
    turn(degrees, Beat::Observe);
    message = "Turning toward the target";
    return;
  }
  corrections = 0;
  if (!z.rangeValid || z.rangeAgeMs >= 250) {
    if (!objects) message = "Person found; waiting for the distance sensor";
    if (now-at > 4500) outcome("Cannot measure the path ahead", false, "OBSTACLE");
    return;
  }
  const int stopAt = task == Command::FindMe ? kFindMeStopMm : task == Command::Chase ? kChaseStopMm : 550;
  if (closeArrival && z.distanceMm > stopAt + 20) closeArrival = false;
  if (z.distanceMm <= stopAt || closeArrival) {
    if (task == Command::Chase) {
      if (!chaseNear) {
        chaseNear = true;
        present("HAPPY", labels[targetClass], AudioCue::Happy, Beat::Observe, "Caught up; watching your object");
      }
    } else celebrate();
    return;
  }
  if (task == Command::Chase && chaseNear && z.distanceMm < kChaseResumeMm) return;
  const bool wasNear = chaseNear;
  chaseNear = false;
  present("SERIOUS", objects ? labels[targetClass] : "PERSON", wasNear ? AudioCue::Curious : AudioCue::None,
          Beat::Drive, task == Command::Chase ? "Chasing your object" : "Coming closer");
}
}

namespace action_mission {
bool start(Command value) {
  cancel(); task = value; running = true;
  started = at = millis(); scans = corrections = advances = 0; heading = 0;
  returning = chaseNear = exitTarget = sawTarget = false;
  closeArrival = false;
  targetClass = value >= Command::FindBattery && value <= Command::FindScrewdriver ? int(value)-int(Command::FindBattery) : -1;
  if (value == Command::Escape) targetClass = 1; // Marker placed at the course exit.
  limitMs = value == Command::Identify ? 12000 : value == Command::Chase ? 60000 : 150000;
  if (!visionMode(value != Command::FindMe)) { running = false; message = "Detection model unavailable"; return false; }
  present("CURIOUS", value == Command::Chase ? "CHASE" : value == Command::Escape ? "EXIT" : "SEARCHING",
          AudioCue::Curious, Beat::Observe, value == Command::Identify ? "Let me look" : "Looking for the target");
  return true;
}
void cancel() {
  running = false; beat = Beat::Idle; stopVision();
}
bool active() { return running; }
const char* phase() { return message; }
void poll() {
  if (!running) return;
  const uint32_t now = millis();
  const auto z = zumo_link::snapshot();
  const auto v = vision_service::snapshot();
  if (ownsVision && (!v.enabled || v.batteryMode != objects)) {
    outcome("Detection was changed; action stopped"); return;
  }
  if (now-started > limitMs && beat != Beat::Finish && !(beat == Beat::Present && afterPresent == Beat::Finish)) {
    outcome(task == Command::Chase ? "Chase finished" : "Search time limit reached", false, "LOST"); return;
  }
  switch (beat) {
    case Beat::Present:
      if (!issued) {
        if (stopped(z) && zumo_link::scene(expression, caption)) { issued = true; at = now; }
        else if (now-at > 2500) { running = false; stopVision(); zumo_link::stop(); message = "Display could not start"; }
        return;
      }
      if (!cueIssued) {
        if (!strcmp(z.sceneResult, "SENDING")) return;
        if (strcmp(z.sceneResult, "APPLIED")) { running = false; stopVision(); zumo_link::stop(); message = "Display update failed"; return; }
        if (cue != AudioCue::None && !audioEngine.playCue(cue)) {
          running = false; stopVision(); zumo_link::stop(); message = "Voice unavailable"; return;
        }
        cueIssued = true; shownAt = now;
      }
      if (now-shownAt < (afterPresent == Beat::Finish || afterPresent == Beat::Return ? 900U : 180U)) return;
      if ((afterPresent == Beat::Finish || afterPresent == Beat::Return) && audioEngine.cuePlaying() && now-shownAt < 2500) return;
      beat = afterPresent; at = now; issued = false;
      if (beat == Beat::Observe) observe();
      if (beat == Beat::Drive) drive();
      break;
    case Beat::Observe: inspect(v, z, now); break;
    case Beat::Turn:
      if (!issued) {
        if (stopped(z) && zumo_link::look(turnAngle, z.controlGeneration)) { issued = true; at = now; }
        else if (now-at > 2000) outcome("Turn could not start");
        return;
      }
      if (z.lookActive) return;
      if (strcmp(z.lookResult, "DONE")) { outcome("Turn was interrupted"); return; }
      heading = normalized(heading + turnAngle);
      beat = afterTurn; at = now;
      if (beat == Beat::Observe) observe();
      if (beat == Beat::EscapeRead) { rangeSamples = rangeTotal = 0; rangeReply = 0; }
      break;
    case Beat::Drive: {
      const bool course = task == Command::Escape && !exitTarget;
      const bool frameFresh = freshFrame(v, now);
      const auto* box = frameFresh ? select(v) : nullptr;
      const bool aligned = box && abs((box->x1+box->x2)/2-v.imageWidth/2) <= v.imageWidth/8;
      const int stopAt = course ? 350 : task == Command::FindMe ? kFindMeStopMm : task == Command::Chase ? kChaseStopMm : 550;
      const bool closeApproach = task == Command::FindMe || task == Command::Chase;
      if (closeApproach && z.approachArrived) closeArrival = true;
      if (!z.rangeValid || z.rangeAgeMs >= 250 || z.distanceMm <= stopAt ||
          closeArrival || !frameFresh || (!course && !aligned) ||
          now-at >= driveFor) {
        brake(Beat::Observe); return;
      }
      if (!lastDrive || now-lastDrive >= 80) {
        if (!zumo_link::safeDrive(driveSpeed, driveSpeed, closeApproach ? stopAt : 150)) {
          outcome("Approach was rejected", false, "OBSTACLE"); return;
        }
        lastDrive = now;
      }
      break;
    }
    case Beat::Brake:
      if (stopped(z)) {
        beat = afterBrake; at = now; issued = false;
        if (beat == Beat::Observe) observe();
      }
      else if (now-at > 1200) outcome("Motors did not stop");
      break;
    case Beat::Return:
      returning = true; sawTarget = false; scans = corrections = 0;
      if (!visionMode(false)) { outcome("Person detector unavailable"); return; }
      targetClass = -1;
      turn(normalized(-heading), Beat::Observe);
      message = "Turning back to find you";
      break;
    case Beat::EscapeRead: rangeRead(z, now); break;
    case Beat::Celebrate:
      if (!issued) {
        if (stopped(z) && zumo_link::wiggle(z.controlGeneration, true)) { issued = true; at = now; }
        else if (now-at > 2500) outcome("Found the target", true, "FOUND");
        return;
      }
      if (z.lookActive) {
        if (!cheering && !strcmp(z.lookResult, "WIGGLING")) {
          if (!audioEngine.playCue(AudioCue::Excited)) { outcome("Celebration voice unavailable"); return; }
          cheering = true;
        }
        if (!centerChirp && !strcmp(z.lookResult, "CENTERING")) {
          if (!audioEngine.playCue(AudioCue::Neutral)) { outcome("Celebration voice unavailable"); return; }
          centerChirp = cheering = true;
        }
        return;
      }
      audioEngine.cancelOutput();
      if (strcmp(z.lookResult, "DONE")) { outcome("Celebration interrupted"); return; }
      present("EXCITED", task == Command::Escape ? "EXIT" : "FOUND", AudioCue::None, Beat::Finish, message);
      break;
    case Beat::Finish:
      stopVision(); audioEngine.cancelOutput(); running = false; beat = Beat::Idle;
      break;
    default: break;
  }
}
}
