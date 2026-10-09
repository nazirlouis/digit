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
std::string m2Reply(uint32_t boot,uint32_t session,unsigned job,const char* phase,
                    const char* reason,int distance=500,int heading=0,int rangeStatus=0,int rawDistance=500) {
  char body[256],frame[300];
  snprintf(body,sizeof(body),"M2 %08lx %lu %u %s %s %d %d 25 GAP=8 OVERRUN=0 DSUS=1490 TL=1579 TR=1579 RS=%d RAW=%d",
      static_cast<unsigned long>(boot),static_cast<unsigned long>(session),job,phase,reason,distance,heading,rangeStatus,rawDistance);
  snprintf(frame,sizeof(frame),"0 %s *%04X",body,motionCrc(body,strlen(body)));
  return frame;
}
int main() {
  zumo_link::begin();
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=-1 RANGE=0 AUTO=1 RA=20 RS=2 RAW=24 NR=1",nowMs);
  assert(zumo_link::snapshot().rangeNoReturn && !zumo_link::snapshot().rangeValid);
  assert(zumo_link::snapshot().distanceMm==-1 && zumo_link::snapshot().rangeAgeMs==20);
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=350 RANGE=1 AUTO=1 BRAKE=1 SAFE2=1 CLOSE=2 RA=220 RS=0 RAW=350 ENC=1 VL=1540 VR=1535",nowMs);
  assert(zumo_link::snapshot().rangeAgeMs==220);
  assert(!zumo_link::snapshot().rangeNoReturn); // No stale inheritance from an old reply.
  assert(zumo_link::snapshot().encoderControl && zumo_link::snapshot().encoderLeftCps==1540);
  assert(zumo_link::snapshot().rangeStatus==0 && zumo_link::snapshot().rawDistanceMm==350);
  nowMs+=35;
  assert(zumo_link::snapshot().rangeAgeMs==255);
  assert(!zumo_link::safeDrive(3000,3000,50));
  handleLine("0 EVENT ENCODER",nowMs);
  assert(!zumo_link::snapshot().ready);
  assert(!strcmp(zumo_link::snapshot().fault,"ENCODER"));
  handleLine("0 EVENT OBSTACLE",nowMs);
  assert(!strcmp(zumo_link::snapshot().lastError,"OBSTACLE"));
  handleLine("0 EVENT TIMEOUT",nowMs);
  assert(!strcmp(zumo_link::snapshot().lastError,"TIMEOUT"));
  zumo_link::stop(); zumo_link::poll();
  reply(false);
  assert(!zumo_link::snapshot().encoderControl); // Legacy peer cannot inherit stale capability.
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
  reply(); assert(zumo_link::drive(1,-2500)); nowMs+=100; zumo_link::poll();
  assert(zumoUart.outgoing.find(" DRIVE 3000 -3000 6000\n")!=std::string::npos);
  reply(); assert(zumo_link::turn(-1)); nowMs+=100; zumo_link::poll();
  assert(zumoUart.outgoing.find(" TURN -3000\n")!=std::string::npos);
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
  assertCommand("SAFE 3000 3000 500","DRIVE 3000 3000 500");
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
  assertCommand("SAFE 3000 3000 500 100","SAFE 3000 3000 500");
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
  assertCommand("SAFE 3000 3000 500 50","SAFE 3000 3000 500 100");
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
  assert(zumo_link::safeDrive(2400,2400,100));
  assert(zumo_link::safeDrive(2400,2400,50));
  handleLine("333 OK ARRIVED",nowMs);
  assert(zumo_link::snapshot().approachArrived && !driveWanted);
  zumo_link::stop(); zumo_link::poll(); handleLine(closePeer.c_str(),nowMs);
  assert(!zumo_link::snapshot().chaseCloseCapable && !zumo_link::safeDrive(2400,2400,50));
  for(int target : {50,100,150}) {
    zumo_link::stop();zumo_link::poll();
    auto boundary=chasePeer;
    boundary.replace(boundary.find("DIST=900"),8,"DIST=49");
    handleLine(boundary.c_str(),nowMs);
    assert(!zumo_link::safeDrive(2400,2400,target));
    assert(zumo_link::drive(4000,4000,500)); // Manual has no front-distance gate.
    zumo_link::stop();zumo_link::poll();
    boundary.replace(boundary.find("DIST=49"),7,"DIST=50");
    handleLine(boundary.c_str(),nowMs);
    assert(zumo_link::safeDrive(2400,2400,target));
  }
  std::cout<<"PASS 5 cm chase capability, strictly below 5 cm obstacle guard, manual exemption, local arrival and 10 cm peer compatibility\n";
  std::cout<<"PASS explicit close-approach negotiation, distance argument, local arrival and no automatic restart\n";
  std::cout<<"PASS quick wiggle negotiation, generation, single command, heartbeat, completion and cancellation\n";
  std::cout<<"PASS production UART bridge: default 6000, BRAKE support, older peer fallback, edge and lease priority\n";
  zumo_link::stop(); zumo_link::poll();
  nowMs+=100;
  assert(motionCrc("123456789",9)==0x29b1); // Standard CCITT-FALSE test vector.
  reply();zumo_link::superviseMotion();assert(!zumo_link::armMotion());
  assert(!motionWanted); // No autonomous fallback to an older controller.
  const std::string jobs=std::string(modern)+" CLOSE=2 ENC=1 VL=0 VR=0 MOTION=1 BOOT=1234abcd SESSION=9 GAP=12 OVERRUN=2 DSUS=4300 TL=0 TR=0";
  handleLine(jobs.c_str(),nowMs);zumo_link::superviseMotion();
  assert(zumo_link::snapshot().controlMaxGapMs==12 && zumo_link::snapshot().controlOverruns==2 && zumo_link::snapshot().displayMaxSliceUs==4300);
  assert(zumo_link::snapshot().motionCapable && !zumo_link::armMotion());
  assert(motionSession==10); // A restarted XIAO advances the peer's high-water mark.
  zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find("M2 1234abcd 10 0 ARM *")!=std::string::npos);
  auto armed=m2Reply(0x1234abcd,10,0,"ARMED","NONE");
  auto broken=armed;broken.back()=broken.back()=='0'?'1':'0';
  handleLine(broken.c_str(),nowMs);assert(!zumo_link::snapshot().motionArmed);
  nowMs+=200;zumo_link::superviseMotion();zumo_link::poll();
  assert(motionWanted && armPending); // Lost ACK retries exactly the same session.
  handleLine(armed.c_str(),nowMs);assert(zumo_link::armMotion());
  auto job=zumo_link::driveJob(3000,3000,50,30000);assert(job==1);
  zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find("M2 1234abcd 10 1 DRIVE 3000 3000 500 50 30000 *")!=std::string::npos);
  auto accepted=m2Reply(0x1234abcd,10,job,"RUNNING","ACCEPTED");
  handleLine(accepted.c_str(),nowMs);
  zumoUart.outgoing.clear();
  for(int i=0;i<10;++i){nowMs+=100;zumo_link::superviseMotion();zumo_link::poll();}
  assert(zumoUart.outgoing.find(" DRIVE ")==std::string::npos);
  assert(zumoUart.outgoing.find(" 0 HB 2 *")!=std::string::npos);
  assert(zumoUart.outgoing.find(" 0 HB 11 *")!=std::string::npos);
  assert(zumo_link::snapshot().motionActive); // Browser lease does not govern jobs.
  assert(zumo_link::updateDriveJob(job,3100,3000));zumo_link::poll();
  assert(zumoUart.outgoing.find("1 UPDATE 1 3100 3000 *")!=std::string::npos);
  assert(zumo_link::updateDriveJob(job,3000,3050));zumo_link::poll();
  assert(zumoUart.outgoing.find("1 UPDATE 2 3000 3050 *")!=std::string::npos);
  const auto previousUpdateWire=motionWires[(motionWireCursor-1)%16].wire;
  // A correction queued just before arrival belongs only to this drive job.
  assert(zumo_link::updateDriveJob(job,3200,3000));
  auto done=m2Reply(0x1234abcd,10,job,"DONE","WALL_STOPPED",52,7);
  broken=done;broken.back()=broken.back()=='0'?'1':'0';handleLine(broken.c_str(),nowMs);
  assert(zumo_link::snapshot().motionActive);
  handleLine(m2Reply(0x1234abcd,9,job,"DONE","WALL_STOPPED").c_str(),nowMs);
  assert(zumo_link::snapshot().motionActive); // An old session cannot finish this job.
  handleLine(done.c_str(),nowMs);handleLine(accepted.c_str(),nowMs);
  auto completion=zumo_link::snapshot();
  assert(!completion.motionActive && !strcmp(completion.motionResult,"WALL_STOPPED"));
  assert(completion.motionDistanceMm==52 && completion.motionHeadingDeciDeg==7);
  assert(completion.controlMaxGapMs==8 && completion.displayMaxSliceUs==1490 && completion.targetLeftCps==1579);
  assert(zumo_link::ackMotion(job));
  auto measure=zumo_link::measureJob();assert(measure==2);
  zumoUart.outgoing.clear();zumo_link::poll();zumo_link::poll();zumo_link::poll();
  assert(zumoUart.outgoing.find(" UPDATE ")==std::string::npos);
  assert(zumoUart.outgoing.find(" 2 MEASURE 2500 *")!=std::string::npos);
  const auto beforeOldJobError=zumo_link::snapshot().safetyEvents;
  handleLine((std::to_string(previousUpdateWire)+" ERR M2_ERR_STALE").c_str(),nowMs);
  assert(zumo_link::snapshot().motionActive && motionWanted);
  assert(zumo_link::snapshot().safetyEvents==beforeOldJobError);
  handleLine(done.c_str(),nowMs);assert(zumo_link::snapshot().motionJob==2 && zumo_link::snapshot().motionActive);
  handleLine(m2Reply(0x1234abcd,10,measure,"DONE","MEASURED",750,905).c_str(),nowMs);
  completion=zumo_link::snapshot();
  assert(completion.motionSample==measure && completion.motionDistanceMm==750 && completion.motionHeadingDeciDeg==905);
  assert(completion.motionRangeStatus==0 && completion.motionRawDistanceMm==500);
  assert(zumo_link::scene("CURIOUS","SEARCHING"));zumo_link::poll();
  const auto visibleId=sceneId;
  handleLine((std::to_string(visibleId)+" OK SCENE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().sceneResult,"RENDERING"));
  handleLine((std::to_string(visibleId)+" OK SCENE_VISIBLE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().sceneResult,"APPLIED"));
  zumo_link::stop();zumo_link::poll();handleLine(accepted.c_str(),nowMs);
  assert(!zumo_link::snapshot().motionActive && !motionWanted);
  zumo_link::poll(); // A late RUNNING event prompts an additional safe STOP.
  // A living UART task must stop a dead mission, not heartbeat it forever.
  handleLine(jobs.c_str(),nowMs);zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  auto session=motionSession;handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  job=zumo_link::driveJob(3000,3000,50);zumo_link::poll();
  handleLine(m2Reply(0x1234abcd,session,job,"RUNNING","ACCEPTED").c_str(),nowMs);
  zumoUart.outgoing.clear();nowMs+=601;zumo_link::poll();
  assert(!motionWanted && !strcmp(zumo_link::snapshot().lastError,"SUPERVISOR_TIMEOUT"));
  assertCommand("STOP","BRAKE");assert(zumoUart.outgoing.find(" HB ")==std::string::npos);
  // A peer restart invalidates pending jobs and previous-boot events.
  handleLine(jobs.c_str(),nowMs);zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  session=motionSession;handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  const auto restarted=std::string(modern)+" ENC=1 MOTION=1 BOOT=1234abce SESSION=0";
  handleLine(restarted.c_str(),nowMs);handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  assert(!motionWanted && !zumo_link::snapshot().motionArmed);
  assert(!strcmp(zumo_link::snapshot().lastError,"PEER_RESTART"));
  zumo_link::poll();
  // The mission task may queue ARM after the UART iteration began but before
  // it takes stateLock. Its new timestamp must not wrap to a false 49-day age.
  handleLine(jobs.c_str(),nowMs);zumo_link::superviseMotion();
  zumoUart.outgoing.clear();
  beforeCritical=[] { ++nowMs; assert(!zumo_link::armMotion()); };
  zumo_link::poll();
  assert(motionWanted && armPending);
  assert(zumoUart.outgoing.find(" 0 ARM *")!=std::string::npos);
  assert(strcmp(zumo_link::snapshot().lastError,"JOB_ACK_TIMEOUT"));
  // The same race must not expire a freshly renewed supervisor lease.
  beforeCritical=[] { ++nowMs; zumo_link::superviseMotion(); };
  zumo_link::poll();assert(motionWanted);
  // A status update arriving while a caller waits for stateLock is age zero,
  // not UINT32_MAX. Real timeout and wraparound behavior remains unchanged.
  beforeCritical=[] { ++nowMs; handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=350 RANGE=1 AUTO=1",nowMs); };
  assert(zumo_link::snapshot().replyAgeMs==0);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS shared clocks sampled after state lock: queued ARM, supervisor lease and fresh snapshot races\n";
  handleLine((std::string(modern)+" REACT=12345678 MOTION=1 BOOT=1234abce SESSION=0").c_str(),nowMs);
  zumo_link::poll();
  assert(zumo_link::snapshot().reactionVersion==0x12345678);
  const auto presentationControl=zumo_link::snapshot().controlGeneration;
  assert(zumo_link::scene("HAPPY","NONE"));
  zumo_link::cancelScene();zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find("SCENE")==std::string::npos);
  assert(zumo_link::snapshot().controlGeneration==presentationControl);
  assert(zumo_link::scene("CURIOUS","NONE"));zumo_link::poll();
  const auto staleScene=sceneId;assert(staleScene);
  zumo_link::cancelScene();
  handleLine((std::to_string(staleScene)+" OK SCENE_VISIBLE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().sceneResult,"CANCELLED"));
  assert(zumo_link::wake());zumo_link::poll();const auto staleWake=wakeCommandId;
  handleLine((std::to_string(staleWake)+" OK WAKE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().wakeResult,"RENDERING") && wakeCommandId==staleWake);
  zumo_link::cancelWake();
  handleLine((std::to_string(staleWake)+" OK WAKE_VISIBLE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().wakeResult,"CANCELLED"));
  assert(zumo_link::wake());zumo_link::poll();const auto freshWake=wakeCommandId;
  handleLine((std::to_string(freshWake)+" OK WAKE").c_str(),nowMs);
  handleLine((std::to_string(freshWake)+" OK WAKE_VISIBLE").c_str(),nowMs);
  assert(!strcmp(zumo_link::snapshot().wakeResult,"APPLIED") && !wakeCommandId);
  // Later face reactions clean up wake work but must retain its actual result.
  zumo_link::cancelWake();
  assert(!strcmp(zumo_link::snapshot().wakeResult,"APPLIED"));
  assert(zumo_link::wake());
  zumo_link::cancelWake();
  assert(!strcmp(zumo_link::snapshot().wakeResult,"CANCELLED") && !wakePending);
  assert(zumo_link::wake());zumo_link::poll();const auto failedWake=wakeCommandId;
  handleLine((std::to_string(failedWake)+" ERR WAKE_IO").c_str(),nowMs);
  zumo_link::cancelWake();
  assert(!strcmp(zumo_link::snapshot().wakeResult,"WAKE_IO"));
  handleLine(modern,nowMs);assert(zumo_link::snapshot().reactionVersion==0);
  assert(!zumo_link::snapshot().circleCapable && !zumo_link::circleJob(4000));
  handleLine((jobs+" CIRCLE=1").c_str(),nowMs);
  assert(!zumo_link::snapshot().circleCapable); // One-track peer must be upgraded.
  handleLine((jobs+" CIRCLE=2").c_str(),nowMs);
  assert(zumo_link::snapshot().circleCapable);
  zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  session=motionSession;handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  assert(!zumo_link::circleJob(2999) && !zumo_link::circleJob(6001));
  assert(zumo_link::circleJob(5200));zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" CIRCLE 5200 3 ")!=std::string::npos);
  assert(zumoUart.outgoing.find(" 60000 *")!=std::string::npos);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS negotiated circle job, selected speed, three laps and bounded deadline\n";
  std::cout<<"PASS reaction catalog negotiation, independent presentation cancellation, stale acknowledgments and wake visibility\n";
  std::cout<<"PASS persistent M2 jobs, CRC, retry, monotonic sessions, completion latching, heading-specific measurement, independent heartbeat, supervisor timeout and restart cancellation\n";
}
