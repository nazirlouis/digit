// Exercise the production UART bridge, including upgrade compatibility and
// priority of immediate safety stops over queued normal braking requests.
#include "zumo_link.cpp"
#include <cassert>
#include <iostream>

void reply(bool brakeCapable=true) {
  handleLine(brakeCapable
      ? "1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 BRAKE=1"
      : "1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1",nowMs);
  zumoUart.outgoing.clear();
}
void assertCommand(const char* expected, const char* forbidden) {
  const std::string line=" "+std::string(expected)+"\n";
  const std::string other=" "+std::string(forbidden)+"\n";
  assert(zumoUart.outgoing.find(line)!=std::string::npos);
  assert(zumoUart.outgoing.find(other)==std::string::npos);
}
int main() {
  zumo_link::begin();
  reply(false);
  const auto motorStatusAt=zumo_link::snapshot().statusTimestampMs;
  nowMs+=25;handleLine("2 OK STOPPED",nowMs);
  assert(zumo_link::snapshot().statusTimestampMs==motorStatusAt);
  nowMs+=25;reply(false);
  assert(zumo_link::snapshot().statusTimestampMs==nowMs);
  assert(!zumo_link::snapshot().brakeCapable);
  zumo_link::stop(true); zumo_link::poll();
  assertCommand("STOP","BRAKE");
  reply();
  assert(zumo_link::snapshot().brakeCapable);
  zumo_link::stop(true); zumo_link::poll();
  assertCommand("BRAKE","STOP");
  reply();
  assert(zumo_link::drive(4000,4000));
  nowMs+=100; zumo_link::poll();
  assert(zumoUart.outgoing.find(" DRIVE 4000 4000 6000\n")!=std::string::npos);
  reply();
  assert(zumo_link::drive(0,0)); zumo_link::poll();
  assertCommand("BRAKE","STOP");
  reply();
  zumo_link::stop(); zumo_link::poll();
  assertCommand("STOP","BRAKE");
  reply();
  zumo_link::stop(true);
  handleLine("0 EVENT EDGE",nowMs);
  zumo_link::poll();
  assertCommand("STOP","BRAKE");
  reply();
  zumo_link::stop(true);
  nowMs+=2000; zumo_link::poll();
  assertCommand("STOP","BRAKE");
  reply();
  assert(zumo_link::drive(4000,4000));
  nowMs+=250; zumo_link::poll();
  assertCommand("STOP","BRAKE");
  reply(false); // A restarted older peer must remove previously advertised support.
  assert(!zumo_link::snapshot().brakeCapable);
  assert(!zumo_link::look(15,zumo_link::snapshot().controlGeneration));
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 BRAKE=1 LOOKRAMP=1",nowMs);
  assert(zumo_link::snapshot().rampedLookCapable);
  assert(!zumo_link::setAcceleration(499)&&!zumo_link::setAcceleration(6001));
  assert(zumo_link::setAcceleration(500));
  assert(zumo_link::look(15,zumo_link::snapshot().controlGeneration));
  zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" LOOK 15 500\n")!=std::string::npos);
  handleLine((std::to_string(lookId)+" OK LOOKING").c_str(),nowMs);
  assert(zumo_link::setAcceleration(6000)); // In-progress turn retains its request's ramp.
  assert(lookAcceleration==500);
  nowMs+=7000;
  handleLine("1 OK READY=1 FAULT=NONE L=-2400 R=2400 BAT=5800 EDGE=0 VOICE=1 LOOKRAMP=1",nowMs);
  zumo_link::poll();assert(zumo_link::snapshot().lookActive);
  zumo_link::stop();zumo_link::poll();assert(!zumo_link::snapshot().lookActive);
  reply(false);assert(!zumo_link::snapshot().rampedLookCapable);
  assert(!zumo_link::wiggle(zumo_link::snapshot().controlGeneration));
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 BRAKE=1 LOOKRAMP=1 WIGGLE=2",nowMs);
  assert(zumo_link::snapshot().wiggleCapable);
  const auto gestureGeneration=zumo_link::snapshot().controlGeneration;
  assert(!zumo_link::wiggle(gestureGeneration-1));
  assert(zumo_link::wiggle(gestureGeneration));
  zumoUart.outgoing.clear();zumo_link::poll();
  assertCommand("WIGGLE","LOOK 15 6000");
  const auto gestureId=lookId;
  handleLine((std::to_string(gestureId)+" OK LOOKING").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().lookResult,"PREPARING"));
  handleLine((std::to_string(gestureId)+" OK WIGGLE_START").c_str(),nowMs);
  assert(zumo_link::snapshot().lookActive && !strcmp(zumo_link::snapshot().lookResult,"WIGGLING"));
  assert(!zumo_link::wiggle(gestureGeneration));
  nowMs+=100;zumo_link::poll();
  assert(zumoUart.outgoing.find(" KEEP "+std::to_string(gestureId)+"\n")!=std::string::npos);
  handleLine((std::to_string(gestureId)+" OK WIGGLE_CENTER").c_str(),nowMs);
  assert(zumo_link::snapshot().lookActive && !strcmp(zumo_link::snapshot().lookResult,"CENTERING"));
  handleLine((std::to_string(gestureId)+" OK LOOK_DONE").c_str(),nowMs);
  assert(!zumo_link::snapshot().lookActive && !strcmp(zumo_link::snapshot().lookResult,"DONE"));
  assert(zumo_link::wiggle(gestureGeneration));
  zumo_link::stop();zumoUart.outgoing.clear();zumo_link::poll();
  assertCommand("STOP","WIGGLE"); // Cancel before transmission.
  handleLine((std::to_string(gestureId)+" OK WIGGLE_START").c_str(),nowMs);
  assert(!zumo_link::snapshot().lookActive);
  assert(zumo_link::wiggle(zumo_link::snapshot().controlGeneration));
  zumo_link::poll();
  handleLine((std::to_string(lookId)+" OK LOOKING").c_str(),nowMs);
  nowMs+=5001;
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 LOOKRAMP=1 WIGGLE=2",nowMs);
  zumoUart.outgoing.clear();zumo_link::poll();
  assert(!zumo_link::snapshot().lookActive);
  assertCommand("STOP","WIGGLE");
  reply();assert(!zumo_link::snapshot().wiggleCapable);
  assert(!zumo_link::wiggle(zumo_link::snapshot().controlGeneration));
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 LOOKRAMP=1 WIGGLE=1",nowMs);
  assert(!zumo_link::snapshot().wiggleCapable); // Older wiggle has no phase synchronization.
  const char* modern="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 BRAKE=1 LOOKRAMP=1 WIGGLE=2 SCENE=1 SAFE2=1";
  handleLine(modern,nowMs);
  assert(zumo_link::snapshot().sceneCapable && zumo_link::snapshot().autoRampCapable);
  assert(zumo_link::scene("HAPPY","SCREWDRIVER"));
  assert(!zumo_link::safeDrive(2800,2800)); // Never start before LCD acknowledgement.
  zumoUart.outgoing.clear(); zumo_link::poll();
  assertCommand("SCENE HAPPY SCREWDRIVER","WIGGLE");
  auto displayedId=sceneId;
  handleLine((std::to_string(displayedId)+" OK SCENE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().sceneResult,"APPLIED"));
  assert(zumo_link::setAcceleration(500));
  assert(!zumo_link::safeDrive(-2800,2800) && !zumo_link::safeDrive(3201,3201));
  assert(zumo_link::safeDrive(2800,2800)); nowMs+=100; zumoUart.outgoing.clear(); zumo_link::poll();
  assertCommand("SAFE 2800 2800 500","DRIVE 2800 2800 500");
  zumo_link::stop(); zumo_link::poll(); handleLine(modern,nowMs);
  nowMs+=251; handleLine("555 OK KEEP",nowMs);
  assert(zumo_link::snapshot().replyAgeMs==0 && zumo_link::snapshot().rangeAgeMs==251);
  assert(!zumo_link::safeDrive(2800,2800)); // Other ACKs cannot refresh an old distance.
  handleLine(modern,nowMs);
  assert(zumo_link::scene("CONFUSED","LOST")); zumo_link::poll(); displayedId=sceneId;
  zumo_link::stop(); zumoUart.outgoing.clear(); zumo_link::poll();
  assertCommand("STOP","SCENE CONFUSED LOST");
  handleLine((std::to_string(displayedId)+" OK SCENE").c_str(),nowMs);
  assert(sceneId==0 && !driveWanted);
  assert(zumo_link::scene("CURIOUS","SEARCHING")); zumo_link::poll();
  nowMs+=1001; handleLine(modern,nowMs); zumo_link::poll();
  assert(!strcmp(zumo_link::snapshot().sceneResult,"NO_REPLY"));
  assert(zumo_link::wiggle(zumo_link::snapshot().controlGeneration,true));
  zumoUart.outgoing.clear(); zumo_link::poll(); assertCommand("WIGGLE EXCITED","WIGGLE");
  zumo_link::stop(); zumo_link::poll();
  reply(); assert(!zumo_link::snapshot().sceneCapable && !zumo_link::snapshot().autoRampCapable);
  assert(!zumo_link::scene("HAPPY","BATTERY") && !zumo_link::safeDrive(2800,2800));
  std::cout<<"PASS action scene acknowledgements, timeout, Stop, capabilities, selected SAFE ramp and actual range age\n";
  handleLine(modern,nowMs);
  assert(!zumo_link::safeDrive(2400,2400,100));
  const auto closePeer=std::string(modern)+" CLOSE=1";
  handleLine(closePeer.c_str(),nowMs);
  assert(zumo_link::snapshot().closeApproachCapable);
  assert(!zumo_link::snapshot().chaseCloseCapable && !zumo_link::safeDrive(2400,2400,50));
  assert(!zumo_link::safeDrive(2400,2400,20));
  assert(zumo_link::safeDrive(2400,2400,100));
  nowMs+=100; zumoUart.outgoing.clear(); zumo_link::poll();
  assertCommand("SAFE 2400 2400 500 100","SAFE 2400 2400 500");
  const auto safetyBefore=zumo_link::snapshot().safetyEvents;
  handleLine("0 EVENT ARRIVED",nowMs);
  assert(zumo_link::snapshot().approachArrived && !driveWanted);
  assert(zumo_link::snapshot().safetyEvents==safetyBefore);
  assert(!zumo_link::safeDrive(2400,2400,100));
  zumo_link::stop(); zumo_link::poll();
  assert(!zumo_link::snapshot().approachArrived);
  assert(zumo_link::safeDrive(2400,2400,100));
  handleLine("333 OK ARRIVED",nowMs);
  assert(zumo_link::snapshot().approachArrived && !driveWanted);
  zumo_link::stop(); zumo_link::poll(); reply();
  assert(!zumo_link::snapshot().closeApproachCapable);
  const auto chasePeer=std::string(modern)+" CLOSE=2";
  handleLine(chasePeer.c_str(),nowMs);
  assert(zumo_link::snapshot().closeApproachCapable && zumo_link::snapshot().chaseCloseCapable);
  assert(zumo_link::safeDrive(2400,2400,100));
  zumo_link::stop(); zumo_link::poll();
  assert(zumo_link::safeDrive(2400,2400,50));
  nowMs+=100; zumoUart.outgoing.clear(); zumo_link::poll();
  assertCommand("SAFE 2400 2400 500 50","SAFE 2400 2400 500 100");
  handleLine("0 EVENT ARRIVED",nowMs);
  assert(zumo_link::snapshot().approachArrived && !driveWanted);
  assert(zumo_link::snapshot().safetyEvents==safetyBefore);
  assert(!zumo_link::safeDrive(2400,2400,50));
  zumo_link::stop(); zumo_link::poll();
  auto closeRange=chasePeer;
  closeRange.replace(closeRange.find("DIST=900"),8,"DIST=30");
  handleLine(closeRange.c_str(),nowMs);
  assert(!zumo_link::safeDrive(2400,2400,50));
  closeRange.replace(closeRange.find("DIST=30"),7,"DIST=60");
  handleLine(closeRange.c_str(),nowMs);
  assert(!zumo_link::safeDrive(2400,2400,100));
  assert(zumo_link::safeDrive(2400,2400,50));
  handleLine("333 OK ARRIVED",nowMs);
  assert(zumo_link::snapshot().approachArrived && !driveWanted);
  zumo_link::stop(); zumo_link::poll(); handleLine(closePeer.c_str(),nowMs);
  assert(!zumo_link::snapshot().chaseCloseCapable && !zumo_link::safeDrive(2400,2400,50));
  std::cout<<"PASS 5 cm chase capability, 3 cm hard guard, local arrival and 10 cm peer compatibility\n";
  std::cout<<"PASS explicit close-approach negotiation, distance argument, local arrival and no automatic restart\n";
  std::cout<<"PASS quick wiggle negotiation, generation, single command, heartbeat, completion and cancellation\n";
  std::cout<<"PASS production UART bridge: default 6000, BRAKE support, older peer fallback, edge and lease priority\n";
  zumo_link::stop(); zumo_link::poll();
  nowMs+=100;

}
