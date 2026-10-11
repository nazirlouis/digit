#include <cstring>
#include "robot_actions.cpp"
using C=robot_actions::Command;
void reset(){active=false;command=C::None;phase="Idle";controlEpoch=10;actionResult=robot_actions::Result::None;job=0;expressionUntil=0;nowMs=1000;
 mock::z={};mock::z.connected=mock::z.ready=mock::z.motionCapable=mock::z.encoderControl=mock::z.voiceCapable=mock::z.rangeValid=mock::z.distanceCalibrated=true;
 strcpy(mock::z.fault,"NONE");mock::z.distanceMm=500;mock::stops=mock::travels=mock::turns=mock::supervisions=0;mock::armReady=false;speech_commands::state={};wake_word::state={};}
robot_actions::ToolRequest request(C c,int amount){robot_actions::ToolRequest r;r.command=c;r.amount=amount;return r;}
int main(){robot_actions::begin();reset();
 assert(!robot_actions::startFromLive(request(C::Forward,31),10));assert(!robot_actions::startFromLive(request(C::Backward,11),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,91),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,0),10));assert(!robot_actions::startFromLive(request(C::TurnLeft,90),9));
 mock::z.distanceCalibrated=false;assert(!robot_actions::startFromLive(request(C::Forward,10),10));assert(robot_actions::startFromLive(request(C::TurnRight,90),10));assert(robot_actions::generation()==10);
 robot_actions::poll();assert(!mock::turns);mock::armReady=true;robot_actions::poll();assert(mock::turn==-90);
 assert(robot_actions::setExpression("HAPPY",3000));assert(active && speech_commands::liveActive());
 mock::z.motionJob=1;strcpy(mock::z.motionState,"DONE");mock::z.motionHeadingDeciDeg=-897;robot_actions::poll();auto s=robot_actions::snapshot();assert(!s.active && s.result==robot_actions::Result::Completed && s.turnedDeciDeg==-897);
 reset();assert(robot_actions::startFromLive(request(C::Backward,10),10));mock::armReady=true;robot_actions::poll();assert(mock::travel==-100);
 mock::z.motionJob=1;strcpy(mock::z.motionState,"DONE");mock::z.motionTravelledMm=-99;robot_actions::poll();assert(robot_actions::snapshot().travelledMm==-99);
 reset();assert(robot_actions::startFromLive(request(C::Forward,30),10));robot_actions::stop("Local Stop");assert(!active && !speech_commands::liveActive() && robot_actions::generation()==11);mock::armReady=true;robot_actions::poll();assert(!mock::travels);
 reset();assert(robot_actions::startFromLive(request(C::Forward,30),10));mock::z.safetyEvents++;robot_actions::poll();assert(!active && mock::stops==1);
 reset();speech_commands::state.liveActive=false;robot_actions::manualOverride();robot_actions::manualOverride();assert(mock::stops==0 && robot_actions::generation()==12); // Manual heartbeat must not brake/reset every 100 ms.
 reset();assert(robot_actions::setExpression("HAPPY",500));nowMs+=501;robot_actions::poll();assert(!strcmp(mock::expression,"NEUTRAL"));
 reset();speech_commands::state.dryRun=true;assert(!robot_actions::startFromLive(request(C::Forward,30),10));
 puts("PASS: local movement bounds/readiness/calibration, ARM acknowledgement, signed travel/gyro results, silent independent expressions, safety/Stop generation and manual lease continuity");}
