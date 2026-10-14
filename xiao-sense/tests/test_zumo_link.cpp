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
void gestureAudioTests() {
  const char* peer="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 SCENE=1 EXPR=1";
  uint32_t generation=0; uint8_t expression=0;
  auto read=[&]{zumo_link::gestureAudio(generation,expression);};
  auto prepare=[&]{
    zumo_link::stop();zumo_link::poll();nowMs+=20;
    status.motionArmed=false;status.motionActive=false;
    handleLine(peer,nowMs);
    assert(zumo_link::scene("HAPPY","NONE",3000));zumo_link::poll();
    const auto wire=sceneId;
    handleLine((std::to_string(wire)+" OK SCENE_VISIBLE").c_str(),nowMs);
    read();assert(!expression); // Visibility alone cannot trigger sound.
    return wire;
  };
  auto start=[&](unsigned wire,const char* face="HAPPY") {
    handleLine((std::to_string(wire)+" EVENT GESTURE START "+face).c_str(),nowMs);read();
  };
  auto wire=prepare();start(wire+1);assert(!expression);
  start(wire,"SAD");assert(!expression);
  start(wire);assert(expression==2);const auto once=generation;
  start(wire);assert(generation==once); // Repeated notification never replays.
  handleLine((std::to_string(wire+1)+" EVENT GESTURE END HAPPY").c_str(),nowMs);
  read();assert(expression==2);
  handleLine((std::to_string(wire)+" EVENT GESTURE END HAPPY").c_str(),nowMs);
  read();assert(!expression && generation!=once);start(wire);assert(!expression);
  wire=prepare();start(wire);const auto fullPhrase=generation;
  nowMs+=600;handleLine(peer,nowMs);
  handleLine((std::to_string(wire)+" EVENT GESTURE END HAPPY DONE").c_str(),nowMs);
  read();assert(expression==2 && generation==fullPhrase);
  nowMs+=600;handleLine(peer,nowMs);read();assert(expression==2);
  nowMs+=501;handleLine(peer,nowMs);read();assert(!expression);
  wire=prepare();start(wire);
  handleLine((std::to_string(wire)+" EVENT GESTURE END HAPPY CANCEL").c_str(),nowMs);
  read();assert(!expression);
  for (int reason=0;reason<7;++reason) {
    wire=prepare();start(wire);assert(expression==2);
    if(reason==0)zumo_link::stop();
    if(reason==1)zumo_link::cancelScene();
    if(reason==2)handleLine("0 EVENT EDGE",nowMs);
    if(reason==3)nowMs+=501;
    if(reason==4)assert(zumo_link::drive(3000,3000));
    if(reason==5)handleLine((std::string(peer)+" MOTION=1 BOOT=99 SESSION=0").c_str(),nowMs);
    if(reason==6)assert(zumo_link::scene("SAD","NONE",3000));
    read();assert(!expression);start(wire);assert(!expression);
  }
  wire=prepare();nowMs+=1500;handleLine(peer,nowMs);start(wire);assert(!expression);
  wire=prepare();strcpy(status.fault,"SENSOR");start(wire);assert(!expression);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS approved phrase continues after natural gesture completion, legacy/cancel End and all safety/Stop/replacement paths cancel immediately\n";
}
void wakeAudioTests() {
  const char* peer="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 SCENE=1 EXPR=1 MOTION=1 BOOT=42 SESSION=0";
  uint32_t generation=0; uint8_t expression=0;
  auto read=[&]{zumo_link::gestureAudio(generation,expression);};
  auto request=[&]{zumo_link::stop();zumo_link::poll();nowMs+=20;
    status.motionArmed=false;status.motionActive=false;handleLine(peer,nowMs);
    assert(zumo_link::wake());zumo_link::poll();assert(wakeCommandId);read();assert(!expression);return wakeCommandId;};
  auto wire=request();
  handleLine((std::to_string(wire+1)+" OK WAKE_VISIBLE").c_str(),nowMs);read();assert(!expression);
  handleLine((std::to_string(wire)+" OK WAKE").c_str(),nowMs);read();assert(!expression);
  handleLine((std::to_string(wire)+" OK WAKE_VISIBLE").c_str(),nowMs);read();assert(expression==14);
  const auto once=generation;
  handleLine((std::to_string(wire)+" OK WAKE_VISIBLE").c_str(),nowMs);read();assert(generation==once);
  nowMs+=500;handleLine(peer,nowMs);read();assert(expression==14);
  nowMs+=461;handleLine(peer,nowMs);read();assert(!expression);
  wire=request();zumo_link::cancelWake();
  handleLine((std::to_string(wire)+" OK WAKE_VISIBLE").c_str(),nowMs);read();assert(!expression);
  wire=request();handleLine((std::to_string(wire)+" OK WAKE_VISIBLE").c_str(),nowMs);
  read();assert(expression==14);zumo_link::cancelWake();read();assert(!expression);
  wire=request();handleLine((std::to_string(wire)+" OK WAKE_VISIBLE").c_str(),nowMs);
  handleLine("0 EVENT WAKE_FAILED",nowMs);read();assert(!expression);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS wake acknowledgment waits for visible face, plays once, rejects stale replies and cancels on Stop/wake failure\n";
}
void stillEmotionAudioTests() {
  const char* peer="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=900 RANGE=1 AUTO=1 SCENE=1 EXPR=1 MOTION=1 BOOT=42 SESSION=0";
  uint32_t generation=0; uint8_t expression=0;
  auto read=[&]{zumo_link::gestureAudio(generation,expression);};
  auto request=[&](const char* name,uint32_t duration=3000) {
    zumo_link::stop();zumo_link::poll();nowMs+=20;
    status.motionArmed=false;status.motionActive=false;handleLine(peer,nowMs);
    assert(zumo_link::scene(name,"NONE",duration));zumoUart.outgoing.clear();zumo_link::poll();
    assert(zumoUart.outgoing.find(" DRIVE ")==std::string::npos);
    const auto wire=sceneId;read();assert(!expression);
    handleLine((std::to_string(wire)+" OK SCENE").c_str(),nowMs);read();assert(!expression);
    return wire;
  };
  auto visible=[&](unsigned wire){handleLine((std::to_string(wire)+" OK SCENE_VISIBLE").c_str(),nowMs);read();};
  for (const char* name : {"NEUTRAL","SERIOUS","SLEEPING"}) {
    auto wire=request(name);
    handleLine((std::to_string(wire)+" EVENT GESTURE START "+name).c_str(),nowMs);read();
    assert(!expression); // Still emotions never depend on a motor-start event.
    visible(wire+1);assert(!expression);
    visible(wire);assert(expression==faceIndex(name));const auto once=generation;
    visible(wire);assert(expression==faceIndex(name) && generation==once);
    zumo_link::stop();read();assert(!expression);visible(wire);assert(!expression);
    wire=request(name);zumo_link::stop();visible(wire);assert(!expression);
    wire=request(name);nowMs+=1500;visible(wire);assert(!expression);
    wire=request(name);handleLine("0 EVENT EDGE",nowMs);visible(wire);assert(!expression);
    wire=request(name);assert(zumo_link::drive(3000,3000));visible(wire);assert(!expression);
  }
  // Internal neutral resets (expiry, navigation and Stop) are unbounded SCENE
  // or legacy FACE requests, and must never generate a fresh neutral chirp.
  auto wire=request("NEUTRAL",0);visible(wire);assert(!expression);
  assert(zumo_link::face("NEUTRAL"));zumo_link::poll();
  handleLine((std::to_string(nextId-1)+" OK FACE").c_str(),nowMs);read();assert(!expression);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS all three still emotions sound once after visibility, no drive commands, stale/safety/Stop/manual cancellation and silent automatic resets\n";
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
std::string calibrationStatus(unsigned id=1, bool ready=false, const char* fault="EDGE", unsigned boot=0x42) {
  return std::to_string(id)+" OK READY="+(ready ? "1" : "0")+" FAULT="+fault+
      " L=0 R=0 BAT=5800 EDGE="+(!strcmp(fault,"EDGE") ? "1" : "0")+
      " VOICE=1 DIST=500 RANGE=1 AUTO=1 ENC=1 VL=0 VR=0 MOTION=1 BOOT="+
      (boot==0x42 ? "42" : "43")+" SESSION=0";
}
void danceCueTests() {
  zumo_link::stop();zumo_link::poll();
  constexpr uint32_t boot=0x55aa00bb;
  handleLine("1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=500 RANGE=1 AUTO=1 ENC=1 VL=0 VR=0 MOTION=1 BOOT=55aa00bb SESSION=0 DANCE=1",nowMs);
  zumo_link::poll();zumo_link::superviseMotion();assert(status.danceCapable);
  assert(!zumo_link::armMotion());zumo_link::poll();
  handleLine(m2Reply(boot,motionSession,0,"ARMED","NONE").c_str(),nowMs);
  assert(zumo_link::armMotion());const auto job=zumo_link::danceJob();assert(job);
  zumo_link::poll();uint32_t generation=0;uint8_t expression=0;zumo_link::gestureAudio(generation,expression);assert(!expression);
  handleLine(m2Reply(boot,motionSession,job,"RUNNING","DANCING").c_str(),nowMs);
  zumo_link::gestureAudio(generation,expression);assert(expression==15);const auto first=generation;
  handleLine(m2Reply(boot,motionSession,job,"RUNNING","DANCING").c_str(),nowMs);
  zumo_link::gestureAudio(generation,expression);assert(expression==15&&generation==first);
  handleLine(m2Reply(boot,motionSession,job,"DONE","DANCE_DONE").c_str(),nowMs);
  zumo_link::gestureAudio(generation,expression);assert(!expression);
  handleLine(m2Reply(boot,motionSession,job,"RUNNING","DANCING").c_str(),nowMs);
  zumo_link::gestureAudio(generation,expression);assert(!expression);
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS dance music starts once from a correlated actual pulse, ends on completion, and cannot replay\n";
}
void resetCalibration() {
  zumo_link::stop();zumo_link::poll();nowMs+=10;
  discardMotion("CANCELLED");
  handleLine(calibrationStatus().c_str(),nowMs);
  zumoUart.outgoing.clear();
}
uint16_t startCalibration() {
  assert(zumo_link::calibrateSurface());
  zumo_link::poll(); // Stationary STOP precedes sampling.
  zumo_link::poll();
  assert(calibrationId && calibrationActive);
  assert(zumoUart.outgoing.find(" CALIBRATE\n")!=std::string::npos);
  assert(zumoUart.outgoing.find(" ARM ")==std::string::npos && zumoUart.outgoing.find(" DRIVE ")==std::string::npos);
  return calibrationId;
}
void calibrationTests() {
  resetCalibration();const auto first=startCalibration();
  handleLine("0 EVENT READY",nowMs);assert(!calibrationVerifyPending && status.calibrating);
  handleLine((std::to_string(first)+" OK CALIBRATING").c_str(),nowMs);
  handleLine("0 EVENT READY",nowMs);assert(!calibrationVerifyPending); // Too early for 60 samples.
  nowMs+=600;handleLine(calibrationStatus().c_str(),nowMs);handleLine("0 EVENT READY",nowMs);
  zumo_link::poll();const auto verification=calibrationVerifyId;assert(verification);
  handleLine(calibrationStatus(verification+1,true,"NONE").c_str(),nowMs);
  assert(status.calibrating && strcmp(status.calibrationResult,"COMPLETED"));
  handleLine(calibrationStatus(verification,false,"NONE").c_str(),nowMs);
  assert(status.calibrating && strcmp(status.calibrationResult,"COMPLETED")); // Stale READY is not success.
  nowMs+=100;handleLine("0 EVENT READY",nowMs);zumo_link::poll();assert(calibrationVerifyId);
  handleLine(calibrationStatus(calibrationVerifyId,true,"NONE").c_str(),nowMs);
  assert(!status.calibrating && status.ready && !strcmp(status.calibrationResult,"COMPLETED"));

  resetCalibration();assert(zumo_link::calibrateSurface());const auto generation=status.calibrationGeneration;
  zumo_link::stop();zumo_link::poll();assert(!calibrationPending && !calibrationActive);
  assert(zumoUart.outgoing.find(" CALIBRATE\n")==std::string::npos);
  handleLine((std::to_string(first)+" OK CALIBRATING").c_str(),nowMs);handleLine("0 EVENT READY",nowMs);
  assert(!status.calibrating && !strcmp(status.calibrationResult,"CANCELLED") && status.calibrationGeneration==generation);

  resetCalibration();startCalibration();const auto current=calibrationId;zumo_link::stop();zumo_link::poll();
  handleLine((std::to_string(current)+" OK CALIBRATING").c_str(),nowMs);handleLine("0 EVENT READY",nowMs);
  assert(!status.calibrating && !calibrationId && !strcmp(status.calibrationResult,"CANCELLED"));

  resetCalibration();assert(zumo_link::calibrateSurface());zumo_link::poll();zumoUart.outgoing.clear();
  beforeCritical=[] { beforeCritical=[] { zumo_link::stop(); }; };
  zumo_link::poll();assert(zumoUart.outgoing.find(" CALIBRATE\n")==std::string::npos);
  assert(!strcmp(status.calibrationResult,"CANCELLED"));
  resetCalibration();assert(zumo_link::calibrateSurface());zumo_link::poll();zumoUart.outgoing.clear();
  beforeCritical=[] { beforeCritical=[] { status.encoderLeftCps=100; }; };
  zumo_link::poll();assert(zumoUart.outgoing.find(" CALIBRATE\n")==std::string::npos);
  assert(!strcmp(status.calibrationResult,"MOVING"));

  resetCalibration();const auto rejected=startCalibration();
  handleLine((std::to_string(rejected)+" OK CALIBRATING").c_str(),nowMs);
  const auto safety=status.safetyEvents;handleLine("0 EVENT CAL_FAILED",nowMs);
  assert(!status.ready && !status.calibrating && status.safetyEvents==safety+1 && !strcmp(status.calibrationResult,"CAL_FAILED"));

  resetCalibration();const auto timeout=startCalibration();
  handleLine((std::to_string(timeout)+" OK CALIBRATING").c_str(),nowMs);
  nowMs+=3000;handleLine(calibrationStatus().c_str(),nowMs);zumo_link::poll();
  assert(!calibrationActive && !strcmp(status.calibrationResult,"TIMEOUT"));
  handleLine("0 EVENT READY",nowMs);assert(!strcmp(status.calibrationResult,"TIMEOUT"));

  resetCalibration();startCalibration();nowMs+=2001;zumo_link::poll();
  assert(!calibrationPending && !calibrationActive && !strcmp(status.calibrationResult,"DISCONNECTED"));
  resetCalibration();startCalibration();nowMs+=251;handleLine("1 OK PONG",nowMs);zumo_link::poll();
  assert(!strcmp(status.calibrationResult,"STATUS_STALE"));
  resetCalibration();startCalibration();handleLine(calibrationStatus(1,false,"NONE",0x43).c_str(),nowMs);
  assert(!calibrationActive && !strcmp(status.calibrationResult,"PEER_RESTART"));

  for(int guard=0;guard<12;++guard){
    resetCalibration();
    switch(guard){
      case 0:nowMs+=251;break;case 1:status.encoderControl=false;break;
      case 2:status.encoderLeftCps=51;break;case 3:status.encoderRightCps=-51;break;
      case 4:status.left=1;break;case 5:status.targetRightCps=1;break;
      case 6:status.motionArmed=true;break;case 7:status.motionActive=true;break;
      case 8:driveWanted=true;break;case 9:status.lookActive=true;break;
      case 10:stopPending=true;break;case 11:status.calibrating=true;break;
    }
    assert(!zumo_link::calibrateSurface());
  }
  for(const char* fault:{"BUTTON_C","SENSOR","ENCODER","TURN_FAULT"}){
    resetCalibration();strcpy(status.fault,fault);assert(!zumo_link::calibrateSurface());
  }
  resetCalibration();strcpy(status.fault,"CAL_FAILED");assert(zumo_link::calibrateSurface());
  resetCalibration();strcpy(status.fault,"BUTTON_C");assert(zumo_link::calibrate()); // Dashboard retains explicit recovery.
  zumo_link::stop();zumo_link::poll();
  std::cout<<"PASS stationary calibration: EDGE, strict rest/fault gates, ordered acknowledgement/status verification, stale replies, Stop, failure, timeout and disconnect\n";
}
void edgeRetreatTests() {
 const std::string peer="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=500 RANGE=1 AUTO=1 BRAKE=1 ENC=1 VL=0 VR=0 MOTION=1 BOOT=1234abcd SESSION=0 TRAVEL=1 DCAL=1";
 auto start=[&](bool capable=true,bool permit=true,bool full=false,bool direct=false) {
   zumo_link::stop();zumo_link::poll();nowMs+=10;
   handleLine((peer+(capable?" ERET=1":"")+(full?" E100=1":"")+(direct?" EDIRECT=1":"")).c_str(),nowMs);
   zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
   handleLine(m2Reply(0x1234abcd,motionSession,0,"ARMED","NONE").c_str(),nowMs);
   assert(zumo_link::travelJob(100,15000,permit));zumoUart.outgoing.clear();zumo_link::poll();
 };
 auto edgeFrame=[&](const char* phase,const char* reason,int mask=1,int back=0) {
   char body[256],packet[300];
   snprintf(body,sizeof(body),"M2 1234abcd %lu %lu %s %s 500 0 10 TL=0 TR=0 TRAVEL=24 ER=%d BACK=%d",
      static_cast<unsigned long>(motionSession),static_cast<unsigned long>(status.motionJob),phase,reason,mask,back);
   snprintf(packet,sizeof(packet),"0 %s *%04X",body,motionCrc(body,strlen(body)));return std::string(packet);
 };
 start();assert(zumoUart.outgoing.find("15000 1 *")!=std::string::npos);
 const auto safety=status.safetyEvents;
 handleLine(edgeFrame("RECOVERING","EDGE_WAIT").c_str(),nowMs);
 assert(status.motionActive && !status.ready && status.edge && !stopPending && status.safetyEvents==safety);
 handleLine(m2Reply(0x1234abcd,motionSession,status.motionJob,"RUNNING","ACCEPTED").c_str(),nowMs);
 assert(!strcmp(status.motionState,"RECOVERING"));
 const auto terminal=edgeFrame("DONE","EDGE_RETREATED",1,16);
 handleLine(terminal.c_str(),nowMs);assert(status.ready && !status.edge && status.motionRetreatMm==16 && !status.motionActive);
 handleLine(terminal.c_str(),nowMs);assert(status.safetyEvents==safety && status.motionRetreatMm==16);
 zumo_link::stop();zumo_link::poll();handleLine(terminal.c_str(),nowMs);assert(!motionWanted && !status.motionActive);
 start(false);assert(zumoUart.outgoing.find("15000 1 *")==std::string::npos); // Older peer uses normal hard stop.
 start();handleLine(edgeFrame("RECOVERING","EDGE_WAIT").c_str(),nowMs);
 handleLine(m2Reply(0x1234abcd,motionSession,status.motionJob,"DONE","TRAVEL_DONE").c_str(),nowMs);
 assert(!motionWanted && stopPending && !strcmp(status.lastError,"EDGE_PROTOCOL"));
 start();handleLine(m2Reply(0x1234abcd,motionSession,status.motionJob,"RECOVERING","EDGE_WAIT").c_str(),nowMs);
 assert(!motionWanted && stopPending && !strcmp(status.lastError,"EDGE_PROTOCOL"));
 for(int failure=0;failure<5;++failure) {
   start(true,failure!=0);
   const auto before=status.safetyEvents;
   auto packet=edgeFrame("RECOVERING","EDGE_WAIT",failure==1?32:1,failure==2?21:0);
   if(failure==3) handleLine("0 EVENT BUTTON_C",nowMs);
   if(failure==4) packet=edgeFrame("DONE","EDGE_RETREATED",1,0);
   handleLine(packet.c_str(),nowMs);
   assert(!motionWanted && stopPending && status.safetyEvents>before);
   if(failure==3) assert(!status.ready && !strcmp(status.fault,"BUTTON_C"));
 }
 start(true,false,true);const auto fullSafety=status.safetyEvents;
 handleLine(edgeFrame("RECOVERING","EDGE_WAIT").c_str(),nowMs);
 handleLine(edgeFrame("RECOVERING","EDGE_BACK",16,50).c_str(),nowMs);
 assert(motionWanted && !stopPending && status.safetyEvents==fullSafety);
 handleLine(edgeFrame("DONE","EDGE_RETREATED",16,100).c_str(),nowMs);
 assert(status.ready && !status.edge && status.motionRetreatMm==100 && status.safetyEvents==fullSafety);
 for(const int invalid:{20,103}) {
   start(true,false,true);
   handleLine(edgeFrame("DONE","EDGE_RETREATED",16,invalid).c_str(),nowMs);
   assert(!motionWanted && stopPending && !strcmp(status.lastError,"EDGE_PROTOCOL"));
 }
 start(true,false,true,true);
 handleLine(edgeFrame("RECOVERING","EDGE_BACK").c_str(),nowMs);
 handleLine(edgeFrame("DONE","EDGE_RETREATED",16,104).c_str(),nowMs);
 assert(status.edgeDirectReverseCapable && status.ready && status.motionRetreatMm==104 && !stopPending);
 start(true,false,true,true);
 handleLine(edgeFrame("DONE","EDGE_RETREATED",16,111).c_str(),nowMs);
 assert(!motionWanted && stopPending && !strcmp(status.lastError,"EDGE_PROTOCOL"));
 zumo_link::stop();zumo_link::poll();nowMs+=10;
 handleLine((peer+" ERET=1 E100=1").c_str(),nowMs);
 assert(zumo_link::drive(3000,3000));
 handleLine("0 EVENT EDGE_AUTO_START",nowMs);
 assert(status.automaticEdgeRecovery && !driveWanted && !stopPending);
 assert(zumo_link::drive(6000,6000) && !driveWanted && !stopPending);
 handleLine((peer+" ERET=1 E100=1 EREC=0").c_str(),nowMs);
 assert(status.ready && !status.automaticEdgeRecovery);
 assert(zumo_link::drive(6000,6000) && !driveWanted); // Held button cannot replay after recovery.
 zumo_link::stop();assert(stopPending);
 zumo_link::poll();assert(zumo_link::drive(3000,3000) && driveWanted);
 zumo_link::stop();zumo_link::poll();
 std::cout<<"PASS negotiated edge retreat, correlated states/results, old-peer fallback, duplicate/stale replies and concurrent hard faults\n";
}
void localRecoveryAndPushTests() {
 const std::string peer="1 OK READY=1 FAULT=NONE L=0 R=0 BAT=5800 EDGE=0 VOICE=1 DIST=20 RANGE=1 AUTO=1 BRAKE=1 ENC=1 VL=0 VR=0 MOTION=1 BOOT=1234abcd SESSION=0 TRAVEL=1 DCAL=1 ERET=1 EREV=1";
 auto start=[&](bool capable=true) {
   zumo_link::stop();zumo_link::poll();nowMs+=10;
   handleLine((peer+(capable?" RECOVERY=1 PUSH=1":"")).c_str(),nowMs);
   zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
   handleLine(m2Reply(0x1234abcd,motionSession,0,"ARMED","NONE").c_str(),nowMs);
   zumoUart.outgoing.clear();
 };
 auto frame=[&](const char* phase,const char* reason,unsigned recoveries=1,const char* cause="RANGE_STALE",const char* extra="KNOWN=1") {
   char body[256],packet[300];
   snprintf(body,sizeof(body),"M2 1234abcd %lu %lu %s %s -1 25 260 TRAVEL=24 REC=%u CAUSE=%s %s",
       static_cast<unsigned long>(motionSession),static_cast<unsigned long>(status.motionJob),phase,reason,recoveries,cause,extra);
   snprintf(packet,sizeof(packet),"0 %s *%04X",body,motionCrc(body,strlen(body)));return std::string(packet);
 };
 start(false);assert(!status.localRecoveryCapable && !status.pushCapable && !zumo_link::pushJob(100));
 zumo_link::poll();assert(zumoUart.outgoing.find(" DRIVE ")==std::string::npos);
 start();assert(status.localRecoveryCapable && status.pushCapable);
 for(const int mm:{-10,0,9,301}) assert(!zumo_link::pushJob(mm));
 const auto job=zumo_link::pushJob(300);assert(job);zumo_link::poll();
 assert(zumoUart.outgoing.find(" PUSH 300 *")!=std::string::npos && motionEdgeAllowed);
 const auto safety=status.safetyEvents;
 handleLine(frame("PAUSED","RANGE_STALE").c_str(),nowMs);
 assert(status.motionActive && status.motionArmed && !stopPending && status.safetyEvents==safety && !status.edge);
 assert(status.motionRecoveryCount==1 && !strcmp(status.motionRecoveryReason,"RANGE_STALE"));
 handleLine(frame("RUNNING","ACCEPTED",0,"NONE").c_str(),nowMs);
 assert(!strcmp(status.motionState,"PAUSED")); // Stale pre-recovery reply ignored.
 nowMs+=100;zumo_link::superviseMotion();zumoUart.outgoing.clear();zumo_link::poll();
 assert(zumoUart.outgoing.find(" HB ")!=std::string::npos);
 handleLine(frame("RUNNING","ACCEPTED").c_str(),nowMs);assert(status.motionActive);
 const auto done=frame("DONE","PUSH_DONE");handleLine(done.c_str(),nowMs);
 assert(!status.motionActive && status.motionTravelledMm==24 && status.motionRecoveryCount==1 && status.safetyEvents==safety);
 handleLine(frame("PAUSED","RANGE_STALE").c_str(),nowMs);assert(!strcmp(status.motionState,"DONE"));
 zumo_link::stop();zumo_link::poll();handleLine(done.c_str(),nowMs);assert(!motionWanted && !status.motionActive);
 start();assert(zumo_link::travelJob(100,15000,true));
 handleLine(frame("RECOVERING","GYRO_STALE",1,"GYRO_STALE","KNOWN=0").c_str(),nowMs);
 assert(status.motionActive && !status.edge && !stopPending && !status.motionPositionKnown);
 handleLine(frame("RECOVERING","GYRO_STALE",1,"GYRO_STALE","KNOWN=1").c_str(),nowMs);
 assert(!status.motionPositionKnown); // Same-job stale replies cannot restore certainty.
 handleLine("0 EVENT BUTTON_C",nowMs);zumo_link::poll();assert(!motionWanted && !status.motionActive);
 start();assert(zumo_link::travelJob(100,15000,true));
 handleLine(frame("RECOVERING","BLOCK_WAIT",1,"ENCODER_STALL","KNOWN=1 ER=0 BACK=0").c_str(),nowMs);
 assert(status.motionActive && !status.edge && !stopPending && motionBlockedBackoff);
 handleLine(frame("RECOVERING","BLOCK_BACK",1,"ENCODER_STALL","KNOWN=1 ER=0 BACK=10").c_str(),nowMs);
 assert(status.motionRetreatMm==10 && !status.motionEdgeMask && !stopPending);
 handleLine(frame("DONE","BLOCKED_RECOVERED",1,"ENCODER_STALL","KNOWN=1 ER=0 BACK=12").c_str(),nowMs);
 assert(!status.motionActive && status.ready && status.motionRetreatMm==12 && !stopPending);
 // A stall without safe backoff can also settle and request a new plan; it
 // must not be misreported as a physical retreat or rejected for missing ER.
 start();assert(zumo_link::travelJob(100,15000,true));
 handleLine(frame("RECOVERING","ENCODER_STALL",1,"ENCODER_STALL").c_str(),nowMs);
 handleLine(frame("DONE","BLOCKED_RECOVERED",1,"ENCODER_STALL").c_str(),nowMs);
 assert(!status.motionActive && !status.motionRetreatMm && !stopPending);
 start(false);assert(zumo_link::travelJob(100));handleLine(frame("PAUSED","RANGE_STALE").c_str(),nowMs);
 assert(stopPending && !motionWanted && !strcmp(status.lastError,"RECOVERY_PROTOCOL"));
 zumo_link::stop();zumo_link::poll();
 std::cout<<"PASS negotiated bounded PUSH without legacy fallback, local recovery leases, diagnostics, stale reply rejection and hard-stop precedence\n";
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
  for (const auto velocity : {51, -51}) {
    status.encoderLeftCps=velocity;
    assert(!zumo_link::armMotion() && !motionWanted);
    status.encoderLeftCps=0;status.encoderRightCps=velocity;
    assert(!zumo_link::armMotion() && !motionWanted);
    status.encoderRightCps=0;
  }
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
  handleLine(jobs.c_str(),nowMs);
  assert(!zumo_link::snapshot().spinCapable && !zumo_link::spinJob());
  handleLine((jobs+" SPIN=0").c_str(),nowMs);
  assert(!zumo_link::snapshot().spinCapable);
  handleLine((jobs+" SPIN=10").c_str(),nowMs);
  assert(!zumo_link::snapshot().spinCapable);
  handleLine((jobs+" SPIN=1").c_str(),nowMs);
  assert(zumo_link::snapshot().spinCapable);
  zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  session=motionSession;handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  assert(!zumo_link::spinJob(999) && !zumo_link::spinJob(30001));
  job=zumo_link::spinJob();assert(job);
  zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" SPIN 12000 *")!=std::string::npos);
  const auto spinBody=std::string("M2 1234abcd ")+std::to_string(session)+" "+std::to_string(job)+" SPIN 12000 *";
  nowMs+=181;zumo_link::superviseMotion();zumo_link::poll();
  assert(zumoUart.outgoing.find(spinBody)!=std::string::npos); // Retry keeps this job identity.
  handleLine(m2Reply(0x1234abcd,session,job,"RUNNING","ACCEPTED",52,100).c_str(),nowMs);
  handleLine(m2Reply(0x1234abcd,session,job,"DONE","SPIN_DONE",52,3700).c_str(),nowMs);
  handleLine(m2Reply(0x1234abcd,session,job,"RUNNING","ACCEPTED",52,100).c_str(),nowMs);
  assert(!zumo_link::snapshot().motionActive && !strcmp(zumo_link::snapshot().motionResult,"SPIN_DONE"));
  assert(zumo_link::snapshot().motionHeadingDeciDeg==3700 && zumo_link::ackMotion(job));
  zumo_link::stop();zumo_link::poll();
  assert(!zumo_link::spinJob()); // Stop cannot be followed by a queued retry.
  handleLine(jobs.c_str(),nowMs);assert(!zumo_link::snapshot().spinCapable);
  std::cout<<"PASS negotiated single spin, strict capability/deadline, same-job retry, completion latch and Stop\n";
  std::cout<<"PASS reaction catalog negotiation, independent presentation cancellation, stale acknowledgments and wake visibility\n";
  // New measured travel is gated by a distinct capability/calibration, not the old ToF DRIVE.
  zumo_link::stop();zumo_link::poll();
  handleLine((jobs+" TRAVEL=1 DCAL=0 EXPR=1 CL=123 CR=125").c_str(),nowMs);
  assert(!zumo_link::snapshot().distanceCalibrated && !zumo_link::travelJob(100));
  handleLine((jobs+" TRAVEL=1 DCAL=1 EXPR=1 CL=123 CR=125").c_str(),nowMs);
  assert(zumo_link::snapshot().distanceCalibrated && zumo_link::snapshot().encoderLeftCount==123 && zumo_link::snapshot().expressionCapable);
  assert(!zumo_link::travelJob(301) && !zumo_link::travelJob(-101));
  zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  session=motionSession;handleLine(m2Reply(0x1234abcd,session,0,"ARMED","NONE").c_str(),nowMs);
  job=zumo_link::travelJob(-100);assert(job);zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" TRAVEL -100 3000 ")!=std::string::npos);
  std::string body="M2 1234abcd "+std::to_string(session)+" "+std::to_string(job)+" DONE TRAVEL_DONE 500 0 10 TRAVEL=-99";
  char packet[256];snprintf(packet,sizeof(packet),"0 %s *%04X",body.c_str(),motionCrc(body.c_str(),body.size()));
  handleLine(packet,nowMs);assert(zumo_link::snapshot().motionTravelledMm==-99);
  zumo_link::stop();zumo_link::poll();handleLine((jobs+" EXPR=1").c_str(),nowMs);
  assert(zumo_link::scene("HAPPY","NONE",2000));zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" SCENE HAPPY NONE 2000\n")!=std::string::npos);
  // EDGE keeps motion disarmed while all 13 display-only expressions remain usable.
  zumo_link::stop();zumo_link::poll();
  std::string edgePeer=jobs+" TRAVEL=1 DCAL=1 EXPR=1";
  edgePeer.replace(edgePeer.find("READY=1 FAULT=NONE"),strlen("READY=1 FAULT=NONE"),"READY=0 FAULT=EDGE");
  edgePeer.replace(edgePeer.find("EDGE=0"),strlen("EDGE=0"),"EDGE=1");
  handleLine(edgePeer.c_str(),nowMs);
  handleLine("0 EVENT EDGE",nowMs);
  assert(!zumo_link::scene("HAPPY","NONE",2000)); // The immediate safety Stop goes first.
  zumoUart.outgoing.clear();zumo_link::poll();assertCommand("STOP","SCENE HAPPY NONE 2000");
  for (const char* face : kFaceNames) {
    handleLine(edgePeer.c_str(),nowMs);
    assert(zumo_link::scene(face,"NONE",2000));zumoUart.outgoing.clear();zumo_link::poll();
    assert(zumoUart.outgoing.find(" SCENE "+std::string(face)+" NONE 2000\n")!=std::string::npos);
    handleLine((std::to_string(sceneId)+" OK SCENE_VISIBLE").c_str(),nowMs);
    const auto edgeStatus=zumo_link::snapshot();
    assert(!strcmp(edgeStatus.sceneResult,"APPLIED") && edgeStatus.edge && !edgeStatus.ready);
    assert(!strcmp(edgeStatus.fault,"EDGE") && !edgeStatus.left && !edgeStatus.right);
    assert(!zumo_link::drive(3000,3000) && !zumo_link::safeDrive(3000,3000));
    zumo_link::superviseMotion();assert(!zumo_link::armMotion());
    assert(!zumo_link::travelJob(100) && !zumo_link::turnJob(30));
    assert(!zumo_link::wiggle(edgeStatus.controlGeneration));
    assert(!motionWanted && !driveWanted && !lookPending);
  }
  assert(zumo_link::scene("NEUTRAL","NONE"));zumo_link::cancelScene();
  assert(zumo_link::scene("HAPPY","NONE",2000));
  zumo_link::stop();zumoUart.outgoing.clear();zumo_link::poll();
  assertCommand("STOP","SCENE HAPPY NONE 2000");assert(!scenePending && !sceneId);
  handleLine(edgePeer.c_str(),nowMs);
  status.calibrating=true;assert(!zumo_link::scene("HAPPY","NONE",2000));status.calibrating=false;
  strcpy(status.fault,"BUTTON_C");assert(!zumo_link::scene("HAPPY","NONE",2000));
  handleLine(edgePeer.c_str(),nowMs);nowMs+=501;assert(!zumo_link::scene("HAPPY","NONE",2000));
  std::cout<<"PASS all expressions under EDGE, unchanged motion lock, Stop priority, calibration/fault/link guards\n";
  const auto edgeSafetyEvents=status.safetyEvents;
  handleLine((jobs+" TRAVEL=1 DCAL=1 EXPR=1").c_str(),nowMs);
  assert(status.ready && !status.edge && !strcmp(status.fault,"NONE"));
  assert(!strcmp(status.lastError,"NONE") && status.safetyEvents==edgeSafetyEvents);
  assert(!motionWanted && !driveWanted && !lookPending);
  assert(!zumo_link::travelJob(100)); // Restored floor never reopens the old session.
  strcpy(status.lastError,"SENSOR");
  handleLine(jobs.c_str(),nowMs);
  assert(!strcmp(status.lastError,"SENSOR")); // Other diagnostics are not erased.
  std::cout<<"PASS restored floor clears only stale EDGE warning without resuming movement\n";
  std::cout<<"PASS measured travel negotiation/limits/signed result, raw calibration counts and bounded expression duration\n";
  std::cout<<"PASS persistent M2 jobs, CRC, retry, monotonic sessions, completion latching, heading-specific measurement, independent heartbeat, supervisor timeout and restart cancellation\n";
  // The physical edge's M2 result and legacy event preserve one failure identity.
  zumo_link::stop();zumo_link::poll();nowMs+=20;
  handleLine((jobs+" TRAVEL=1 DCAL=1 EREV=1 REV=0").c_str(),nowMs);
  zumo_link::superviseMotion();zumo_link::armMotion();zumo_link::poll();
  const auto edgeSession=motionSession;
  handleLine(m2Reply(status.motionBoot,edgeSession,0,"ARMED","NONE").c_str(),nowMs);
  const auto edgeJob=zumo_link::travelJob(100);assert(edgeJob);zumo_link::poll();
  const auto beforeEdge=status.safetyEvents;
  std::string faultBody="M2 1234abcd "+std::to_string(edgeSession)+" "+std::to_string(edgeJob)+" FAULT EDGE 500 0 10 TRAVEL=30 REV=1 EREJ=ACC_STALE EDETAIL=floor:1024,100,100,100,100;limit:853,853,853,853,853;acc_mg:0,0,1000;age_ms:130,5,5;path_mm:40";
  char faultPacket[768];snprintf(faultPacket,sizeof(faultPacket),"0 %s *%04X",faultBody.c_str(),motionCrc(faultBody.c_str(),faultBody.size()));
  faultPacket[strlen(faultPacket)-1]^=1;
  handleLine(faultPacket,nowMs);assert(!status.edgeRetreatReject[0]);
  faultPacket[strlen(faultPacket)-1]^=1;
  handleLine(faultPacket,nowMs);assert(status.safetyEvents==beforeEdge+1 && status.edgeReverseAllowed);
  assert(!strcmp(status.edgeRetreatReject,"ACC_STALE"));
  assert(strstr(status.edgeRetreatDetail,"age_ms:130,5,5;path_mm:40"));
  assert(!status.motionEdgeMask && !status.motionRetreatMm);

  handleLine("0 EVENT EDGE",nowMs);assert(status.safetyEvents==beforeEdge+1 && status.edgeReverseAllowed);
  assert(!strcmp(status.edgeRetreatReject,"ACC_STALE"));
  handleLine("0 EVENT BUTTON_C",nowMs);assert(status.safetyEvents==beforeEdge+2 && !status.edgeReverseAllowed);
  // Explicitly negotiated reverse-only recovery works across readiness polling.
  zumo_link::stop();zumo_link::poll();nowMs+=20;
  handleLine((edgePeer+" EREV=1 REV=1").c_str(),nowMs);
  assert(status.edgeReverseAllowed && !status.ready);
  assert(!zumo_link::drive(3000,3000) && !zumo_link::drive(-3000,3000) && !zumo_link::drive(-3000,-4000));
  assert(zumo_link::drive(-3000,-3000));zumoUart.outgoing.clear();zumo_link::poll();
  nowMs+=100;handleLine((edgePeer+" EREV=1 REV=1").c_str(),nowMs);zumo_link::poll();
  assert(driveWanted && zumoUart.outgoing.find(" DRIVE -3000 -3000 ")!=std::string::npos);
  zumo_link::stop();zumo_link::poll();nowMs+=20;handleLine((edgePeer+" EREV=1 REV=1").c_str(),nowMs);
  zumo_link::superviseMotion();assert(!zumo_link::armMotion());assert(!motionWanted);
  assert(!zumo_link::armMotion(true));assert(motionWanted);zumo_link::poll();
  assert(!status.edgeRetreatReject[0] && !status.edgeRetreatDetail[0]);
  const auto backSession=motionSession;
  handleLine(m2Reply(status.motionBoot,backSession,0,"ARMED","NONE").c_str(),nowMs);
  assert(!zumo_link::turnJob(20) && !zumo_link::travelJob(100) && !zumo_link::travelJob(-100));
  const auto backJob=zumo_link::edgeBackJob(100);assert(backJob);
  assert(!zumo_link::edgeBackJob(101));zumoUart.outgoing.clear();zumo_link::poll();
  assert(zumoUart.outgoing.find(" BACK 100 *")!=std::string::npos);
  std::string doneBody="M2 1234abcd "+std::to_string(backSession)+" "+std::to_string(backJob)+" DONE EDGE_BACK_DONE 500 0 10 TRAVEL=-99";
  char donePacket[256];snprintf(donePacket,sizeof(donePacket),"0 %s *%04X",doneBody.c_str(),motionCrc(doneBody.c_str(),doneBody.size()));
  handleLine(donePacket,nowMs);assert(status.ready && !status.edge && !status.edgeReverseAllowed && status.motionTravelledMm==-99);
  zumo_link::stop();zumo_link::poll();
  // Old firmware and independent hard faults never grant this exception.
  handleLine(edgePeer.c_str(),nowMs);assert(!status.edgeReverseAllowed && !zumo_link::drive(-3000,-3000));
  handleLine((edgePeer+" EREV=1 REV=1").c_str(),nowMs);handleLine("0 EVENT BUTTON_C",nowMs);
  assert(!status.edgeReverseAllowed && !zumo_link::drive(-3000,-3000));zumo_link::poll();
  std::cout<<"PASS negotiated reverse-only manual and M2 gates, 100 mm bounds, readiness polls, completion and old-peer/hard-fault lock\n";
  calibrationTests();
  edgeRetreatTests();
  localRecoveryAndPushTests();
  gestureAudioTests();
  stillEmotionAudioTests();
  wakeAudioTests();
  danceCueTests();
}
