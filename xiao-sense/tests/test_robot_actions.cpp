#include <cstring>
#include "robot_actions.cpp"
using C=robot_actions::Command;
void reset(){active=false;command=C::None;phase="Idle";controlEpoch=10;actionResult=robot_actions::Result::None;job=0;expressionUntil=0;nowMs=1000;
 recoveryPending=false;failureReason="";jobSession=jobBoot=recoveryAt=recoveryRestAt=recoveryAfter=0;
 recoveryCount=0;actionRecoveryReason[0]=0;edgeAllowed=edgeSeen=reverseAtEdge=false;edgeMask=retreatMm=0;
 mock::z={};mock::z.connected=mock::z.ready=mock::z.motionCapable=mock::z.encoderControl=mock::z.voiceCapable=mock::z.rangeValid=mock::z.distanceCalibrated=true;
 strcpy(mock::z.fault,"NONE");mock::z.statusTimestampMs=nowMs;mock::z.distanceMm=500;mock::stops=mock::travels=mock::turns=mock::supervisions=mock::calibrations=0;mock::calibrationAllowed=true;mock::armReady=false;robot_session::state={};wake_word::state={};}
robot_actions::ToolRequest request(C c,int amount){robot_actions::ToolRequest r;r.command=c;r.amount=amount;return r;}
void blockedForward(const char* reason="TRACK_MISMATCH",C c=C::Forward) {
 reset();mock::z.motionBoot=42;mock::z.motionSession=7;
 assert(robot_actions::startFromLive(request(c,10),10));mock::armReady=true;robot_actions::poll();
 mock::z.motionJob=1;mock::z.motionTimestampMs=nowMs;mock::z.motionTravelledMm=18;
 strcpy(mock::z.motionState,"DISARMED");strcpy(mock::z.motionResult,reason);strcpy(mock::z.lastError,reason);
 ++mock::z.safetyEvents;mock::z.left=mock::z.right=4092;mock::z.encoderLeftCps=603;mock::z.encoderRightCps=187;
}
void edgeAction(bool permit=true) {
 reset();mock::z.motionBoot=42;mock::z.motionSession=7;mock::z.edgeRetreatCapable=true;
 auto r=request(C::Forward,10);r.edgeRetreat=permit;
 assert(robot_actions::startFromLive(r,10));mock::armReady=true;robot_actions::poll();
 mock::z.motionJob=1;mock::z.motionTimestampMs=nowMs;mock::z.motionEdgeMask=17;
 mock::z.motionActive=mock::z.motionArmed=mock::z.edge=true;mock::z.ready=false;
 strcpy(mock::z.motionState,"RECOVERING");strcpy(mock::z.motionResult,"EDGE_WAIT");strcpy(mock::z.fault,"EDGE_RETREAT");
}
void edgeTests() {
 edgeAction();robot_actions::poll();
 assert(active && robot_session::liveActive() && !mock::stops && edgeSeen && mock::supervisions>=2);
 nowMs+=100;mock::z.motionTimestampMs=nowMs;mock::z.motionRetreatMm=16;mock::z.motionTravelledMm=24;
 mock::z.motionActive=mock::z.edge=false;mock::z.ready=true;
 strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,"EDGE_RETREATED");strcpy(mock::z.fault,"NONE");
 robot_actions::poll();auto result=robot_actions::snapshot();
 assert(!result.active && result.result==robot_actions::Result::EdgeRetreated && result.travelledMm==24);
 assert(result.edgeMask==17 && result.retreatMm==16 && robot_actions::generation()==10 && robot_session::liveActive());
 for(int guard=0;guard<10;++guard) {
   edgeAction(guard!=0);
   switch(guard) {
     case 1:++mock::z.safetyEvents;break;case 2:++mock::z.motionBoot;break;
     case 3:++mock::z.motionSession;break;case 4:++mock::z.motionJob;break;
     case 5:mock::z.motionRetreatMm=21;break;case 6:mock::z.motionEdgeMask=0;break;
     case 7:mock::z.connected=false;break;case 8:nowMs+=601;break;
     case 9:strcpy(mock::z.fault,"BUTTON_C");break;
   }
   robot_actions::poll();assert(!active && !robot_session::liveActive() && mock::stops==1);
 }
 edgeAction();robot_actions::poll();robot_actions::stop("User Stop");robot_actions::poll();
 assert(!active && actionResult==robot_actions::Result::Cancelled && !robot_session::liveActive());
 for(const auto c:{C::Forward,C::Backward,C::TurnLeft,C::Push,C::Dance}) {
   reset();mock::z.motionBoot=42;mock::z.motionSession=7;
   mock::z.edgeRetreatCapable=mock::z.edgeRetreat100Capable=mock::z.edgeDirectReverseCapable=mock::z.pushCapable=mock::z.danceCapable=true;
   assert(robot_actions::startFromLive(request(c,c==C::Dance ? 0 : 10),10));
   mock::armReady=true;robot_actions::poll();
   mock::z.motionJob=1;mock::z.motionTimestampMs=nowMs;mock::z.motionEdgeMask=16;
   mock::z.motionArmed=mock::z.motionActive=mock::z.edge=true;mock::z.ready=false;
   mock::z.voiceCapable=false;
   strcpy(mock::z.motionState,"RECOVERING");strcpy(mock::z.motionResult,"EDGE_BACK");strcpy(mock::z.fault,"EDGE_RETREAT");
   robot_actions::poll();assert(active && !mock::stops && robot_session::liveActive());
   mock::z.motionRetreatMm=104;mock::z.motionTravelledMm=-62;
   mock::z.motionActive=mock::z.edge=false;mock::z.ready=true;
   strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,"EDGE_RETREATED");strcpy(mock::z.fault,"NONE");
   robot_actions::poll();assert(!active && actionResult==robot_actions::Result::EdgeRetreated && retreatMm==104);
   assert(travelled==-62 && robot_session::liveActive() && robot_actions::generation()==10);
 }
 puts("PASS: correlated local edge retreat, same task generation, measured outcome and hard-stop precedence");
}
void recoveryTests() {
 for(const char* reason:{"TRACK_MISMATCH","OBSTACLE"}) {
   blockedForward(reason);assert(robot_actions::recoverableMotionFailure(mock::z));robot_actions::poll();
   assert(active && recoveryPending && actionResult==robot_actions::Result::Running && robot_session::liveActive());
   assert(robot_actions::generation()==10 && !mock::stops && mock::travels==1);
   nowMs+=100;mock::z.statusTimestampMs=nowMs;mock::z.left=mock::z.right=0;mock::z.encoderLeftCps=mock::z.encoderRightCps=0;
   robot_actions::poll();assert(active);
   nowMs+=160;robot_actions::poll();assert(active); // Reusing one sample never certifies rest.
   mock::z.statusTimestampMs=nowMs;robot_actions::poll();
   auto result=robot_actions::snapshot();assert(!result.active && result.result==robot_actions::Result::Recoverable);
   assert(result.travelledMm==18 && !strcmp(result.failureReason,reason) && robot_session::liveActive() && !mock::stops);
 }
 for(int guard=0;guard<11;++guard) {
   blockedForward();
   switch(guard) {
     case 0:++mock::z.motionSession;break;case 1:++mock::z.motionJob;break;case 2:++mock::z.motionBoot;break;
     case 3:++mock::z.safetyEvents;break;case 4:mock::z.edge=true;break;
     case 5:strcpy(mock::z.fault,"ENCODER");break;case 6:nowMs+=251;break;
     case 7:mock::z.rangeValid=false;break;case 8:mock::z.motionActive=true;break;
     case 9:mock::z.encoderControl=false;break;case 10:mock::z.rangeAgeMs=250;break;
   }
   assert(!robot_actions::recoverableMotionFailure(mock::z));robot_actions::poll();
   assert(!active && !robot_session::liveActive() && mock::stops==1);
 }
 for(const char* reason:{"RANGE_STALE","GYRO_IO","FEEDBACK_IO","ENCODER_DIRECTION","CANCELLED","JOB_TIMEOUT"}) {
   blockedForward(reason);assert(!robot_actions::recoverableMotionFailure(mock::z));robot_actions::poll();
   assert(!active && !robot_session::liveActive() && mock::stops==1);
 }
 blockedForward("TRACK_MISMATCH",C::Backward);robot_actions::poll();assert(!active && !robot_session::liveActive());
 blockedForward();robot_actions::poll();nowMs+=2000;mock::z.statusTimestampMs=nowMs;robot_actions::poll();
 assert(!active && !robot_session::liveActive() && mock::stops==1);
 blockedForward();robot_actions::poll();++mock::z.safetyEvents;strcpy(mock::z.fault,"BUTTON_C");robot_actions::poll();
 assert(!active && !robot_session::liveActive() && mock::stops==1);
 blockedForward();robot_actions::poll();robot_actions::stop("User Stop");
 mock::z.left=mock::z.right=mock::z.encoderLeftCps=mock::z.encoderRightCps=0;nowMs+=200;mock::z.statusTimestampMs=nowMs;
 robot_actions::poll();assert(actionResult==robot_actions::Result::Cancelled && !robot_session::liveActive());
 puts("PASS: forward blockage classification, correlated events, fresh confirmed rest, partial result and hard-stop precedence");
}
void edgeReverseTests() {
 for (const int reversedMm : {99,25}) {
 blockedForward("EDGE");mock::z.ready=false;mock::z.edge=true;
 mock::z.edgeReverseCapable=mock::z.edgeReverseAllowed=true;strcpy(mock::z.fault,"EDGE");
 strcpy(mock::z.edgeRetreatReject,"ACC_STALE");
 strcpy(mock::z.edgeRetreatDetail,"floor:1024,100,100,100,100;age_ms:130,5,5");
 assert(robot_actions::recoverableMotionFailure(mock::z));robot_actions::poll();
 robot_actions::DebugEvent logged{};
 assert(robot_actions::debugEvent(robot_actions::latestDebugId(),&logged));
 assert(!strcmp(logged.edgeRetreatReject,"ACC_STALE"));
 assert(strstr(logged.edgeRetreatDetail,"age_ms:130,5,5"));
 assert(active && recoveryPending && robot_session::liveActive());
 nowMs+=100;mock::z.statusTimestampMs=nowMs;mock::z.left=mock::z.right=mock::z.encoderLeftCps=mock::z.encoderRightCps=0;
 robot_actions::poll();nowMs+=160;mock::z.statusTimestampMs=nowMs;robot_actions::poll();
 assert(!active && actionResult==robot_actions::Result::Recoverable && !strcmp(failureReason,"EDGE"));
 assert(robot_actions::generation()==10 && robot_session::liveActive());
 assert(!robot_actions::startFromLive(request(C::Forward,10),10));
 assert(!robot_actions::startFromLive(request(C::TurnLeft,20),10));
 assert(robot_actions::startFromLive(request(C::Backward,10),10));robot_actions::poll();
 assert(mock::travel==-100 && reverseAtEdge);
 mock::z.motionJob=1;strcpy(mock::z.motionState,"RUNNING");robot_actions::poll();assert(active);
 mock::z.ready=true;mock::z.edge=false;strcpy(mock::z.fault,"NONE");
 // A stopped, verified recovery may finish short after a jolt. The controller
 // reports actual distance; the supervisor keeps the task for a fresh view.
 mock::z.motionTravelledMm=-reversedMm;strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,"EDGE_BACK_DONE");
 robot_actions::poll();assert(!active && actionResult==robot_actions::Result::Completed && travelled==-reversedMm);
 assert(robot_session::liveActive());
 }
 puts("PASS: edge keeps task open, confirmed rest, bounded reverse and measured full/short recovery completion");
}
void localRecoveryTests() {
 for(const auto c:{C::Forward,C::Backward,C::TurnLeft,C::Push}) {
   reset();mock::z.localRecoveryCapable=mock::z.pushCapable=true;
   mock::z.motionBoot=42;mock::z.motionSession=7;
   assert(robot_actions::startFromLive(request(c,10),10));mock::armReady=true;robot_actions::poll();
   mock::z.motionJob=1;mock::z.motionTimestampMs=nowMs;mock::z.motionArmed=mock::z.motionActive=true;
   mock::z.ready=false;mock::z.rangeValid=false;mock::z.encoderControl=false;
   strcpy(mock::z.motionState,"PAUSED");strcpy(mock::z.motionResult,"RANGE_STALE");
   mock::z.motionRecoveryCount=1;mock::z.motionPositionKnown=false;strcpy(mock::z.motionRecoveryReason,"RANGE_STALE");
   robot_actions::poll();assert(active && robot_session::liveActive() && !mock::stops && recoveryCount==1);
   nowMs+=1000;mock::z.motionTimestampMs=nowMs;
   robot_actions::poll();assert(active && mock::supervisions>=3);
   mock::z.ready=mock::z.rangeValid=mock::z.encoderControl=true;
   strcpy(mock::z.motionState,"RUNNING");robot_actions::poll();assert(active);
   mock::z.motionActive=false;mock::z.motionTravelledMm=98;
   strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,c==C::Push ? "PUSH_DONE":"TRAVEL_DONE");
   robot_actions::poll();const auto done=robot_actions::snapshot();
   assert(!active && done.result==robot_actions::Result::Completed && done.recoveryCount==1 &&
       !strcmp(done.recoveryReason,"RANGE_STALE") && !done.positionKnown && robot_session::liveActive());
 }
 for(const char* outcome:{"BLOCKED_RECOVERED","OBSTACLE_STOPPED","PUSH_BLOCKED"}) {
   reset();mock::z.localRecoveryCapable=mock::z.pushCapable=true;mock::z.motionBoot=42;mock::z.motionSession=7;
   const auto c=!strcmp(outcome,"PUSH_BLOCKED") ? C::Push:C::Forward;
   assert(robot_actions::startFromLive(request(c,10),10));mock::armReady=true;robot_actions::poll();
   mock::z.motionJob=1;mock::z.motionTravelledMm=24;mock::z.motionRecoveryCount=1;
   strcpy(mock::z.motionRecoveryReason,"OBSTACLE");strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,outcome);
   robot_actions::poll();assert(!active && actionResult!=robot_actions::Result::Completed && travelled==24 && robot_session::liveActive());
 }
 reset();mock::z.localRecoveryCapable=mock::z.pushCapable=true;
 assert(robot_actions::startFromLive(request(C::Push,5),10));mock::armReady=true;robot_actions::poll();
 robot_actions::suspendForReconnect();assert(!active && !job && robot_actions::generation()==10 && robot_session::liveActive());
 assert(!strcmp(robot_actions::snapshot().failureReason,"CONNECTION_LOST"));
 robot_actions::stop("User Stop");assert(robot_actions::generation()==11 && !robot_session::liveActive());
 reset();mock::z.motionBoot=42;mock::z.motionSession=7;
 assert(robot_actions::startFromLive(request(C::Forward,10),10));mock::armReady=true;robot_actions::poll();
 mock::z.motionJob=1;mock::z.motionTravelledMm=35;mock::z.motionRecoveryCount=2;
 strcpy(mock::z.motionRecoveryReason,"RANGE_STALE");robot_actions::suspendForReconnect();
 assert(!active && !job && robot_actions::generation()==10 && robot_session::liveActive());
 assert(!positionKnown && travelled==35 && recoveryCount==2 && !strcmp(failureReason,"CONNECTION_LOST"));
 puts("PASS local sensor recovery retains leases and task, reports measured terminal outcome, reconnect stops without stale job replay");
}
void pushTests() {
 reset();assert(!robot_actions::startFromLive(request(C::Push,10),10));
 mock::z.localRecoveryCapable=mock::z.pushCapable=true;
 for(const int distance:{-1,0,31}) assert(!robot_actions::startFromLive(request(C::Push,distance),10));
 mock::z.distanceMm=20;assert(!robot_actions::startFromLive(request(C::Forward,10),10));
 assert(robot_actions::startFromLive(request(C::Push,30),10));mock::armReady=true;robot_actions::poll();
 assert(mock::travel==300 && edgeAllowed && active);
 robot_actions::stop("User Stop");assert(!active && !robot_session::liveActive());
 reset();mock::z.pushCapable=mock::z.localRecoveryCapable=true;mock::z.rangeValid=false;mock::z.rangeNoReturn=true;
 mock::z.rangeAgeMs=250;assert(!robot_actions::startFromLive(request(C::Push,1),10));mock::z.rangeAgeMs=20;
 assert(robot_actions::startFromLive(request(C::Push,1),10));
 puts("PASS bounded forward push requires negotiated support, admits close range/fresh no-return and obeys user Stop");
}
int main(){robot_actions::begin();reset();
 assert(!robot_actions::startFromLive(request(C::Forward,31),10));assert(!robot_actions::startFromLive(request(C::Backward,11),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,91),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,0),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,90),9));
 mock::z.distanceCalibrated=false;assert(!robot_actions::startFromLive(request(C::Forward,10),10));assert(robot_actions::startFromLive(request(C::TurnRight,90),10));assert(robot_actions::generation()==10);
 robot_actions::poll();assert(!mock::turns);mock::armReady=true;robot_actions::poll();assert(mock::turn==-90);
 assert(robot_actions::setExpression("HAPPY",3000));assert(active && robot_session::liveActive());
 mock::z.motionJob=1;strcpy(mock::z.motionState,"DONE");mock::z.motionHeadingDeciDeg=-897;robot_actions::poll();auto s=robot_actions::snapshot();assert(!s.active && s.result==robot_actions::Result::Completed && s.turnedDeciDeg==-897);
 reset();assert(robot_actions::startFromLive(request(C::Backward,10),10));mock::armReady=true;robot_actions::poll();assert(mock::travel==-100);
 mock::z.motionJob=1;strcpy(mock::z.motionState,"DONE");mock::z.motionTravelledMm=-99;robot_actions::poll();assert(robot_actions::snapshot().travelledMm==-99);
 reset();assert(robot_actions::startFromLive(request(C::Forward,30),10));robot_actions::stop("Local Stop");assert(!active && !robot_session::liveActive() && robot_actions::generation()==11);mock::armReady=true;robot_actions::poll();assert(!mock::travels);
 reset();assert(robot_actions::startFromLive(request(C::Forward,30),10));mock::z.safetyEvents++;robot_actions::poll();assert(!active && mock::stops==1);
 reset();robot_session::state.liveActive=false;robot_actions::manualOverride();robot_actions::manualOverride();assert(mock::stops==0 && robot_actions::generation()==12); // Manual heartbeat must not brake/reset every 100 ms.
 reset();assert(robot_actions::setExpression("HAPPY",500));nowMs+=501;robot_actions::poll();assert(!strcmp(mock::expression,"NEUTRAL"));
 // Navigation supersedes an idle expression before trying to arm the tracks.
 reset();assert(robot_actions::setExpression("HAPPY",3000));
 assert(robot_actions::startFromLive(request(C::TurnLeft,10),10));
 assert(!expressionUntil && !strcmp(mock::expression,"NEUTRAL"));
 // Reproduce a greeting while the controller is stopped with a latched edge fault.
 reset();mock::z.ready=false;mock::z.edge=true;strcpy(mock::z.fault,"EDGE");
 assert(robot_actions::setExpression("HAPPY",500));assert(!strcmp(mock::expression,"HAPPY"));
 for (const auto c : {C::Forward,C::Backward,C::TurnLeft,C::TurnRight})
   assert(!robot_actions::startFromLive(request(c,10),10));
 nowMs+=501;robot_actions::poll();assert(!strcmp(mock::expression,"NEUTRAL"));
 assert(!mock::z.ready && mock::z.edge && !strcmp(mock::z.fault,"EDGE"));
 assert(!mock::travels && !mock::turns && !mock::stops && robot_session::liveActive());
 assert(!robot_actions::setExpression("HAPPY",499) && !robot_actions::setExpression("HAPPY",10001));
 mock::z.connected=false;assert(!robot_actions::setExpression("HAPPY",500));mock::z.connected=true;
 mock::z.calibrating=true;assert(!robot_actions::setExpression("HAPPY",500));mock::z.calibrating=false;
 for (const char* fault : {"BUTTON_C","SENSOR","ENCODER","TURN_FAULT","CAL_FAILED"}) {
   strcpy(mock::z.fault,fault);assert(!robot_actions::setExpression("HAPPY",500));
 }
 reset();robot_session::state.dryRun=true;assert(!robot_actions::startFromLive(request(C::Forward,30),10));
 // Push reuses ID 28 but still requires a valid positive amount and capability.
 reset();assert(robot_actions::validCommand(28));
 assert(!robot_actions::startFromLive(request(static_cast<C>(28),0),10));
 assert(!mock::calibrations && !active);
 recoveryTests();
 edgeReverseTests();
 edgeTests();
 localRecoveryTests();
 pushTests();
 reset();mock::z.danceCapable=false;assert(!robot_actions::startFromLive(request(C::Dance,0),10));
 mock::z.danceCapable=true;assert(!robot_actions::startFromLive(request(C::Dance,1),10));
 mock::z.motionBoot=42;mock::z.motionSession=7;
 assert(robot_actions::startFromLive(request(C::Dance,0),10));mock::armReady=true;robot_actions::poll();
 mock::z.motionJob=1;strcpy(mock::z.motionState,"DONE");strcpy(mock::z.motionResult,"DANCE_DONE");
 robot_actions::poll();assert(!active&&actionResult==robot_actions::Result::Completed);
 reset();mock::z.danceCapable=true;strcpy(mock::z.fault,"EDGE");mock::z.edge=true;
 assert(!robot_actions::startFromLive(request(C::Dance,0),10));
 puts("PASS fixed dance amount/capability/safety gates and actual completion");
 puts("PASS: local movement bounds/readiness/calibration, ARM acknowledgement, signed travel/gyro results, silent independent expressions, safety/Stop generation and manual lease continuity");}
