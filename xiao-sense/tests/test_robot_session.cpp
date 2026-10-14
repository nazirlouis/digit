// Actual speech lifecycle + queued tools; network and hardware are mocked.
#include "action_console.cpp"
#include "robot_session.cpp"
using C=robot_actions::Command;
void reset() {
  free(mock::queueStorage); mock::queueStorage=nullptr;mock::queued.clear();inputQueue=nullptr;microphoneStream=1;waitingForVoice=awaitingReadyReaction=false;
  free(previewJpeg); state={}; task={}; previewJpeg=nullptr;previewLock=nullptr;
  queuedText[0]=queuedRecovery[0]=0;reconnectPending=reconnectNoticePrepared=false;
  reconnectAt=reconnectRevision=reconnectStableAt=0;lastExpressionAt=expressionExpiresAt=0;lastExpression[0]=0;
  session=connection=1; connectionRevision=0; generation=opensAt=activationAt=safetyEvents=idleAt=offeredVideoAt=responseProgressAt=lastDashboardTextAt=0; idleMs=30000;
  workerBusy=wasConnected=inputReady=false;
  ownedAction=stopOwned=toolResultReady=dispatching=dispatchCancelled=responsePending=idleArmed=toolStopPending=false;
  observationPending=observationSent=expressionPending=stopExpression=false;
  ownedGeneration=ownedSession=pendingSession=pendingGeneration=pendingAt=0;
  ownedCommand=C::None; zumo_link::status={};
  pendingCommand=0; pendingCall[0]=ownedCall[0]=toolResultCall[0]=0; newInput=newOutput=true;
  mock::now=1000; mock::generation=10; mock::safetyEvents=4; mock::configurationRevision=1;
  mock::connected=mock::actionAvailable=true; mock::actionActive=false;
  mock::queueFull=mock::semaphoreBusy=mock::taskCreationFails=false;
  mock::playing=mock::cuePlaying=mock::chimePlaying=mock::reactionActive=false;
  mock::settings={true,"gemini-test-live",false,30};
  mock::actionAttempts=mock::dispatched=mock::stops=0;
  mock::expressionCancels=0;
  mock::criticalDepth=mock::controlDepth=mock::mutexDepth=0;
  mock::beforeStartHook=mock::afterCriticalExit=mock::afterControlRelease=mock::afterMutexGive=nullptr;
  mock::liveHook=nullptr; mock::liveReturns=false; mock::networkCalls=0;
  camera_service::available=true; camera_service::capturedAt=mock::now; camera_service::copies=0;
  robot_actions::outcome=robot_actions::Result::Running;
  robot_actions::recoveryEligible=false;
  robot_actions::recoveryFailure="TRACK_MISMATCH";
  robot_actions::actualTravelMm=100; robot_actions::actualTurn=200; robot_actions::actualRecoveries=0; robot_actions::positionKnown=true; robot_actions::recoveryCause="";
  robot_session::begin(); assert(state.ready);
}
gemini_live::Callbacks open(Request& r, bool dry=false) {
  assert(robot_session::startLive(mock::generation,false,dry));
  r={connection,mock::configurationRevision,mock::now,session};
  gemini_live::Callbacks cb{&r,cancelled,readAudio,event,action,readVideo,videoSent,cancelTool,pollToolResult,canResume,mediaEpoch,taskActive,readText,readRecovery,readHeartbeat};
  cb.event(gemini_live::Event::Connected,"",cb.context); cb.event(gemini_live::Event::InputReady,"",cb.context); robot_session::poll(); return cb;
}
robot_actions::ToolRequest tool(C c, int amount=0) { robot_actions::ToolRequest r; r.command=c; r.amount=amount; strcpy(r.goal,"Find the battery"); strcpy(r.expression,"HAPPY"); return r; }
void queue(const gemini_live::Callbacks& cb,const char* id,C c,int amount=0) { assert(cb.action(id,tool(c,amount),cb.context)==gemini_live::ActionResult::Queued); robot_session::poll(); }
void observation(const gemini_live::Callbacks& cb) {
  uint8_t jpeg[20]; uint32_t seq=0;
  assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==0); // pre-request frame cannot answer.
  if(observationWaitForRest) {
    ++mock::now;zumo_link::status.statusTimestampMs=mock::now;robot_session::poll();
    camera_service::capturedAt=mock::now;
    assert(!cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context));
    mock::now+=301;zumo_link::status.statusTimestampMs=mock::now;robot_session::poll();
    assert(!observationWaitForRest);
  }
  mock::now+=100; camera_service::capturedAt=mock::now;
  assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4 && !toolResultReady);
  cb.videoSent(jpeg,4,seq,cb.context); robot_session::poll(); assert(toolResultReady);
}
std::string result(const gemini_live::Callbacks& cb, const char* expected) {
  char id[129]{},text[1536]{}; assert(cb.pollToolResult(id,sizeof(id),text,sizeof(text),cb.context)); assert(!strcmp(id,expected)); return text;
}
void prompt(const gemini_live::Callbacks& cb, const char* text="Find the battery") {
  cb.event(gemini_live::Event::UserPrompt,text,cb.context);
  cb.event(gemini_live::Event::InputText,text,cb.context);
}
void taskStart(const gemini_live::Callbacks& cb) { prompt(cb); queue(cb,"start",C::Observe); assert(task.active && observationPending); observation(cb); assert(result(cb,"start").find("observed")!=std::string::npos); }
void blockForward(const gemini_live::Callbacks& cb) {
  queue(cb,"blocked",C::Forward,10);
  strcpy(zumo_link::status.motionState,"RECOVERING"); robot_session::poll();
  assert(state.liveActive && task.active && ownedAction && !observationPending && !toolResultReady);
  assert(!strcmp(state.taskPhase,"Recovering locally"));
  mock::actionActive=false;robot_actions::outcome=robot_actions::Result::BlockedRecovered;
  robot_actions::actualRecoveries=1; robot_actions::recoveryCause="STALL";
  strcpy(zumo_link::status.motionState,"IDLE"); robot_session::poll();
}
std::string completed(const gemini_live::Callbacks& cb,const char* id) {
  mock::actionActive=false;robot_actions::outcome=robot_actions::Result::Completed;
  robot_session::poll();observation(cb);return result(cb,id);
}
std::string finishTask(const gemini_live::Callbacks& cb,const char* outcome) {
  auto finish=tool(C::FinishTask);strcpy(finish.outcome,outcome);strcpy(finish.reason,"Obstacle encountered");
  assert(cb.action("finish",finish,cb.context)==gemini_live::ActionResult::Queued);
  robot_session::poll();return result(cb,"finish");
}
void toolSequencingTests() {
 Request r{}; reset(); auto cb=open(r);
 prompt(cb,"Explore"); queue(cb,"explore",C::Observe);
 assert(task.active&&observationPending&&!toolResultReady);
 const auto deadline=task.started;
 // Reproduce an early observe burst while observe prepares its first
 // image. No rejection may replace the pending call or end the task.
 for(unsigned i=0;i<64;++i) {
  const auto id="early-observe-"+std::to_string(i);
  assert(cb.action(id.c_str(),tool(C::Observe),cb.context)==gemini_live::ActionResult::Busy);
  robot_session::poll();
  assert(state.liveActive&&task.active&&observationPending&&!toolResultReady&&!pendingCommand);
  assert(!strcmp(observationCall,"explore")&&task.started==deadline&&!task.moves&&!mock::dispatched);
 }
 observation(cb);
 assert(cb.action("before-result",tool(C::Observe),cb.context)==gemini_live::ActionResult::Busy);
 assert(result(cb,"explore").find("observed")!=std::string::npos);
 queue(cb,"first-move",C::Forward,10);
 assert(ownedAction&&mock::dispatched==1&&task.active);
 assert(cb.action("during-move",tool(C::Observe),cb.context)==gemini_live::ActionResult::Busy);
 assert(completed(cb,"first-move").find("camera_sent\":true")!=std::string::npos);
 assert(task.active&&state.liveActive&&task.moves==1&&task.started==deadline);
 // Stop must win during image preparation, queued result, and movement.
 for(unsigned phase=0;phase<3;++phase) {
  reset(); cb=open(r); prompt(cb,"Explore"); queue(cb,"explore",C::Observe);
  if(phase>0) observation(cb);
  if(phase==2) { result(cb,"explore"); queue(cb,"move",C::Forward,10); }
  assert(cb.action("stop-now",tool(C::Stop),cb.context)==gemini_live::ActionResult::Queued);
  assert(!state.liveActive&&!task.active&&!toolResultReady&&!observationPending);
  robot_session::poll(); assert(mock::stops==1&&!mock::actionActive);
 }
 puts("PASS: early observation burst preserves task/result, movement continues after completion, Stop preempts every phase");
}
// Let the main-loop task run exactly after the transport clears inputReady.
// A real controller mutex defers that task until the reconnect transition ends.
void reconnectPreemptionTests() {
 for (int connecting=0;connecting<2;++connecting) for (int moving=0;moving<2;++moving) {
  Request r{};reset();auto cb=open(r);taskStart(cb);
  if(moving)queue(cb,"moving",C::Forward,10);
  const auto started=task.started, originalGeneration=mock::generation;
  const std::string goal=state.taskGoal;
  mock::now+=21000;zumo_link::status.statusTimestampMs=mock::now;
  assert(mock::now-activationAt>kActivationWaitMs && !task.expired(mock::now));
  bool interrupted=false;
  std::function<void()> mainLoop;
  mainLoop=[&] {
    if(state.liveActive && task.active && !state.liveConnected && !inputReady) {
      interrupted=true;
      if(mock::controlDepth)mock::afterControlRelease=[] {robot_session::poll();};
      else robot_session::poll();
    } else mock::afterCriticalExit=mainLoop;
  };
  mock::afterCriticalExit=mainLoop;
  cb.event(connecting?gemini_live::Event::Connecting:gemini_live::Event::Error,
      connecting?"Resuming Gemini Live":"Could not send observation prompt",cb.context);
  mock::afterCriticalExit=mock::afterControlRelease=nullptr;
  assert(interrupted && state.liveActive && task.active && reconnectPending);
  assert(task.started==started && state.taskGoal==goal && mock::generation==originalGeneration);
  assert(task.moves==static_cast<unsigned>(moving) && task.recoveries==1);
  assert(!mock::actionActive && mock::stops==1 && !ownedAction && !task.hasObservation);
  assert(!strcmp(state.taskPhase,"Recovering connection") && !state.taskResult[0]);
  assert(cb.action("stale",tool(C::Forward,5),cb.context)==gemini_live::ActionResult::Rejected);
  cb.mediaEpoch(cb.context);
  cb.event(gemini_live::Event::Connected,"Connected",cb.context);
  cb.event(gemini_live::Event::InputReady,"Ready",cb.context);
  ++mock::now;zumo_link::status.statusTimestampMs=mock::now;robot_session::poll();
  mock::now+=301;zumo_link::status.statusTimestampMs=mock::now;robot_session::poll();
  assert(observationPending && !task.hasObservation);
  uint8_t jpeg[20];uint32_t seq=0;mock::now+=100;camera_service::capturedAt=mock::now;
  assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);
  cb.videoSent(jpeg,4,seq,cb.context);robot_session::poll();
  char recovery[1025]{};assert(cb.readRecovery(recovery,sizeof(recovery),cb.context));
  cb.event(gemini_live::Event::RecoverySent,recovery,cb.context);
  assert(!reconnectPending && task.active && task.started==started);
  queue(cb,"next",C::TurnLeft,20);assert(mock::dispatched==static_cast<unsigned>(moving+1));
 }
 puts("PASS: observation-send failure and reconnect rollover survive main-loop preemption after 20 seconds; goal/deadline retained, motion stopped, fresh view required before continuation");
}
void connectionErrorTests() {
 Request r{};reset();auto cb=open(r);taskStart(cb);queue(cb,"moving",C::Forward,10);
 const auto started=task.started, originalGeneration=mock::generation;
 cb.event(gemini_live::Event::Error,"Gemini Live connection lost",cb.context);
 assert(state.liveActive && task.active && reconnectPending && !mock::actionActive && mock::stops==1);
 assert(task.started==started && task.moves==1 && task.recoveries==1 && mock::generation==originalGeneration);
 assert(!ownedAction && !toolResultReady && !task.hasObservation);
 cb.mediaEpoch(cb.context); cb.event(gemini_live::Event::Connecting,"Connecting",cb.context);
 cb.event(gemini_live::Event::Connected,"Connected",cb.context); cb.event(gemini_live::Event::InputReady,"Ready",cb.context);
 ++mock::now; zumo_link::status.statusTimestampMs=mock::now; robot_session::poll();
 mock::now+=301; zumo_link::status.statusTimestampMs=mock::now; robot_session::poll();
 assert(observationPending && !task.hasObservation);
 uint8_t jpeg[20]; uint32_t seq=0; mock::now+=100; camera_service::capturedAt=mock::now;
 assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4); cb.videoSent(jpeg,4,seq,cb.context); robot_session::poll();
 assert(!observationPending && !toolResultReady && reconnectPending && task.hasObservation);
 char text[1025]{}; assert(cb.readRecovery(text,sizeof(text),cb.context));
 assert(strstr(text,"uncertain") && strstr(text,"Do not replay") && task.started==started && task.moves==1);
 assert(cb.action("early",tool(C::Forward,5),cb.context)==gemini_live::ActionResult::Rejected);
 cb.event(gemini_live::Event::RecoverySent,text,cb.context); assert(!reconnectPending && task.active);
 queue(cb,"new-move",C::TurnLeft,20); assert(mock::dispatched==2);
 // Stop, configuration edits, expired task, and a physical fault never resume.
 for(int mode=0;mode<4;++mode) {
  reset();cb=open(r);taskStart(cb);queue(cb,"moving",C::Push,10);
  cb.event(gemini_live::Event::Error,"Gemini Live connection lost",cb.context);
  assert(reconnectPending && !mock::actionActive);
  if(mode==0){ robot_actions::Guard guard; robot_actions::stop("User Stop"); }
  if(mode==1)++mock::configurationRevision;
  if(mode==2)mock::now=task.started+task.deadlineMs;
  if(mode==3)++mock::safetyEvents;
  robot_session::poll(); assert(!state.liveActive && !task.active && !reconnectPending);
  cb.event(gemini_live::Event::Connected,"Connected",cb.context); assert(!cb.readRecovery(text,sizeof(text),cb.context));
  assert(mock::dispatched==1);
 }
 puts("PASS: same-authority reconnect preserves deadline/budgets, stops motion, requires rest and fresh frame; Stop/settings/fault/deadline invalidate resume");
}
void recoveryTests() {
 Request r{};reset();auto cb=open(r);taskStart(cb);const auto started=task.started;
 blockForward(cb);assert(task.active && !task.recoveryRequired && task.recoveries==1 && task.moves==1);
 observation(cb);const auto blocked=result(cb,"blocked");
 assert(blocked.find("blocked_recovered")!=std::string::npos && blocked.find("STALL")!=std::string::npos);
 assert(blocked.find("choose_different_heading")!=std::string::npos && blocked.find("requested_mm\":100")!=std::string::npos);
 assert(finishTask(cb,"completed").find("Detour unfinished")!=std::string::npos && task.active);
 queue(cb,"same-heading",C::Forward,5);assert(mock::dispatched==1 && task.active);result(cb,"same-heading");
 queue(cb,"new-route",C::TurnLeft,20);completed(cb,"new-route");
 assert(!task.headingRequired && task.detourPending);
 // Pushing cannot mark an unfinished navigation detour successful.
 queue(cb,"push",C::Push,5);assert(completed(cb,"push").find("push_completed")!=std::string::npos);
 assert(task.detourPending && task.started==started);
 queue(cb,"around",C::Forward,5);completed(cb,"around"); assert(!task.detourPending);
 // Brief recovery is quiet until terminal; a completed job carries its count.
 queue(cb,"transient",C::Forward,5);strcpy(zumo_link::status.motionState,"PAUSED");
 robot_actions::actualRecoveries=1; robot_actions::recoveryCause="RANGE_STALE";
 robot_session::poll();assert(ownedAction && !toolResultReady && !observationPending && task.active);
 robot_actions::positionKnown=false;
 const auto recovered=completed(cb,"transient");
 assert(recovered.find("completed_after_recovery")!=std::string::npos && recovered.find("position_known\":false")!=std::string::npos && task.recoveries==2);
 // A hard event during local recovery still cancels immediately.
 reset();cb=open(r);taskStart(cb);queue(cb,"moving",C::Forward,10);
 strcpy(zumo_link::status.motionState,"RECOVERING");++mock::safetyEvents;robot_session::poll();
 assert(!state.liveActive && !task.active);robot_session::poll();assert(!mock::actionActive);
 // Older firmware offering a model-directed reverse requires help instead.
 reset();cb=open(r);taskStart(cb);queue(cb,"legacy",C::Forward,10);mock::actionActive=false;
 robot_actions::outcome=robot_actions::Result::Recoverable;robot_session::poll();observation(cb);
 assert(result(cb,"legacy").find("assistance_required")!=std::string::npos && !task.active);
 // Recoveries remain diagnostic: passing the old cap cannot cancel a live job.
 reset();cb=open(r);taskStart(cb);task.recoveries=2;queue(cb,"many-recoveries",C::Forward,10);
 robot_actions::actualRecoveries=2;strcpy(zumo_link::status.motionState,"RECOVERING");
 robot_session::poll();assert(mock::actionActive && task.active && !observationPending && !mock::stops);
 assert(completed(cb,"many-recoveries").find("completed_after_recovery")!=std::string::npos);
 assert(task.active && task.recoveries==4 && task.canRetreatAtEdge());
 // Another obstacle can be replanned after successful progress despite the count.
 queue(cb,"obstacle-again",C::TurnRight,20);mock::actionActive=false;
 robot_actions::actualRecoveries=1;robot_actions::outcome=robot_actions::Result::ObstacleStopped;
 robot_session::poll();observation(cb);
 assert(result(cb,"obstacle-again").find("obstacle_stopped")!=std::string::npos);
 assert(task.active && task.recoveries==5 && task.canMove());
 queue(cb,"replan",C::TurnLeft,20);completed(cb,"replan");
 queue(cb,"push-after-recovery",C::Push,5);assert(mock::actionActive);
 // Reconnection is permitted beyond the old count and still stops old motion.
 cb.event(gemini_live::Event::Error,"Gemini Live connection lost",cb.context);
 assert(task.active && state.liveActive && reconnectPending && !mock::actionActive && task.recoveries>5);
 puts("PASS: firmware recovery remains pending, terminal outcome replans, no model reverse, no same-direction retry, push cannot clear detour, hard stops remain absolute");
}
void edgeTests() {
 Request r{};reset();auto cb=open(r);taskStart(cb);const auto deadline=task.started;
 queue(cb,"edge",C::Forward,10);assert(robot_actions::lastEdgePermit);
 mock::actionActive=false;robot_actions::outcome=robot_actions::Result::EdgeRetreated;
 robot_session::poll();assert(task.active && state.liveActive && observationPending && !toolResultReady);
 assert(task.recoveries==1 && task.edgeRetreats==1 && task.detourPending && task.moves==2 && task.started==deadline && !task.recoveryRequired);
 observation(cb);const auto text=result(cb,"edge");
 assert(text.find("edge_retreated")!=std::string::npos && text.find("edge_sensor_mask\":17")!=std::string::npos);
  assert(text.find("retreated_mm\":100")!=std::string::npos && text.find("task_active\":true")!=std::string::npos);
  assert(text.find("reversed 10.0 cm")!=std::string::npos && text.find("Turn to a different heading")!=std::string::npos);
  assert(text.back()=='}' && text.size()<sizeof(toolResultText));
 queue(cb,"new-route",C::TurnLeft,30);completed(cb,"new-route");
  queue(cb,"continue",C::Forward,10);assert(mock::actionActive && robot_actions::lastEdgePermit);
  completed(cb,"continue");assert(!task.detourPending && task.canRetreatAtEdge());
  queue(cb,"second-edge",C::Forward,10);
  mock::actionActive=false;robot_actions::outcome=robot_actions::Result::EdgeRetreated;
  robot_session::poll();observation(cb);const auto again=result(cb,"second-edge");
  assert(task.active && state.liveActive && task.edgeRetreats==2 && task.started==deadline);
  assert(again.find("reversed 10.0 cm")!=std::string::npos && task.headingRequired);
 puts("PASS: recovered edge reports retreat, preserves task, charges budgets and requires a new heading");
}
void pushTests() {
 Request r{};reset();auto cb=open(r);taskStart(cb);
 zumo_link::status.pushCapable=false;queue(cb,"old-peer",C::Push,10);result(cb,"old-peer");assert(!mock::dispatched && task.active);
 zumo_link::status.pushCapable=true;mock::now+=5001;queue(cb,"old-image",C::Push,10);result(cb,"old-image");assert(!mock::dispatched && task.active);
 queue(cb,"new-image",C::Observe);observation(cb);result(cb,"new-image");
 queue(cb,"push",C::Push,30);assert(mock::dispatchedCommand==28 && !strcmp(state.taskPhase,"Pushing object"));
 assert(completed(cb,"push").find("requested_mm\":300")!=std::string::npos);
 // Fresh no-return is explicit forward clearance, never a sensor failure.
 zumo_link::status.rangeValid=false;zumo_link::status.rangeNoReturn=true;
 queue(cb,"clear",C::Observe);observation(cb);const auto clear=result(cb,"clear");
 assert(clear.find("front_clear_beyond_range\":true")!=std::string::npos && clear.find("front_mm\":-1")!=std::string::npos && task.active);
 puts("PASS: push requires capable peer, fresh view and 1-30 cm request; fresh no-return is beyond-range clearance");
}
void automaticTaskTests() {
  Request r{}; reset(); auto cb=open(r);
  cb.event(gemini_live::Event::UserActivity,"",cb.context);
  cb.event(gemini_live::Event::InputText,"unconfirmed fragment",cb.context);
  cb.event(gemini_live::Event::OutputText,"reply",cb.context);
  cb.event(gemini_live::Event::TurnComplete,"",cb.context);
  assert(!task.active && !mock::dispatched);
  queue(cb,"no-prompt",C::StartTask);
  assert(result(cb,"no-prompt").find("fresh user prompt")!=std::string::npos && !task.active);

  prompt(cb,"Look around");
  assert(task.active && state.taskActive && !task.hasObservation && !observationPending && !mock::dispatched);
  const auto deadline=task.started;
  cb.event(gemini_live::Event::InputText," please",cb.context);
  assert(!strcmp(state.taskGoal,"Look around please") && task.started==deadline);
  // Missing the initial observation is recoverable, with no motor dispatch.
  queue(cb,"too-soon",C::Forward,10);
  assert(result(cb,"too-soon").find("Call observe")!=std::string::npos && task.active && !mock::dispatched);
  queue(cb,"see",C::Observe); observation(cb); result(cb,"see");
  assert(task.hasObservation && !task.observations);
  queue(cb,"move",C::Forward,10);
  assert(robot_session::sendText(mock::generation,"Go left instead"));
  assert(!robot_session::sendText(mock::generation,"queue overflow"));
  char message[1025]{};
  assert(!cb.readText(message,sizeof(message),cb.context));
  completed(cb,"move");
  assert(cb.readText(message,sizeof(message),cb.context)); prompt(cb,message);
  assert(task.started==deadline && task.moves==1 && !strcmp(state.taskGoal,"Go left instead"));
  // Steering and compatibility calls preserve all recovery/deadline limits.
  task.recoveries=2; task.recoveryRequired=true; task.detourPending=true;
  prompt(cb,"Try the other side");
  assert(task.recoveries==2 && task.recoveryRequired && task.detourPending && task.moves==1 && task.started==deadline);
  queue(cb,"legacy",C::StartTask); observation(cb); result(cb,"legacy");
  assert(task.recoveries==2 && task.recoveryRequired && task.detourPending && task.moves==1 && task.started==deadline);
  assert(finishTask(cb,"cancelled").find("cancelled")!=std::string::npos);
  cb.event(gemini_live::Event::InputText,"late fragment",cb.context);
  cb.event(gemini_live::Event::OutputText,"another reply",cb.context);
  cb.event(gemini_live::Event::TurnComplete,"",cb.context);
  queue(cb,"cannot-reopen",C::StartTask); result(cb,"cannot-reopen");
  assert(!task.active);
  mock::now+=10; prompt(cb,"Hello");
  assert(task.active && task.started!=deadline && !task.moves && !task.recoveries && !task.hasObservation);
  finishTask(cb,"completed"); assert(!task.active && state.liveActive);
  prompt(cb,"One more question"); robot_session::cancel();
  prompt(cb,"stale after Stop"); assert(!state.liveActive && !task.active);
  reset(); cb=open(r,true); prompt(cb,"Explore"); queue(cb,"see",C::Observe); observation(cb); result(cb,"see");
  queue(cb,"dry-move",C::Forward,10); observation(cb); result(cb,"dry-move");
  assert(task.active && !mock::dispatched);
  puts("PASS: prompts auto-start tasks, steering preserves budgets, initial vision required, finish and Stop prevent stale restart");
}

void ambientTests() {
 Request r{};reset();auto cb=open(r);uint8_t jpeg[20];uint32_t seq=0;char heartbeat[1025]{};
 assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);cb.videoSent(jpeg,4,seq,cb.context);
 assert(cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context)>0);
 cb.event(gemini_live::Event::HeartbeatSent,"",cb.context);assert(!task.active&&responsePending);
 mock::now+=4200;assert(robot_session::snapshot().waitingForReply&&robot_session::snapshot().waitMs==4200);
 assert(!cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 cb.event(gemini_live::Event::TurnComplete,"",cb.context);
 assert(!robot_session::snapshot().waitingForReply);
 mock::now+=30100;robot_session::poll();assert(state.liveActive&&!task.active);
 camera_service::capturedAt=mock::now;assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);
 queue(cb,"emotion",C::Expression);assert(expressionPending);robot_session::poll();
 assert(result(cb,"emotion").find("Expression visible")!=std::string::npos);
 queue(cb,"repeat",C::Expression);assert(result(cb,"repeat").find("cooldown")!=std::string::npos);
 assert(!mock::dispatched&&!task.active);
 prompt(cb,"Explore");queue(cb,"obs",C::Observe);observation(cb);result(cb,"obs");
 queue(cb,"move",C::Forward,10);assert(mock::dispatched==1);
 mock::now+=100;camera_service::capturedAt=mock::now;assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);
 assert(!cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 robot_session::cancel();assert(!cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context));robot_session::poll();assert(!mock::actionActive);
 reset();cb=open(r,true);queue(cb,"dry-emotion",C::Expression);assert(!expressionPending&&result(cb,"dry-emotion")=="test_only_no_expression_started");
 reset();cb=open(r);prompt(cb,"Dance");queue(cb,"dance-observe",C::Observe);observation(cb);result(cb,"dance-observe");
 task.noProgress=2;queue(cb,"dance",C::Dance);assert(mock::dispatched==1&&robot_session::snapshot().toolPending);
 mock::actionActive=false;robot_actions::outcome=robot_actions::Result::Completed;mock::now+=5000;
 robot_session::poll();observation(cb);assert(result(cb,"dance").find("dance_completed")!=std::string::npos);
 assert(task.moves==1&&task.noProgress==2); // Celebration never renews navigation progress.
 puts("PASS: idle 1 FPS frames, ambient prompts never open tasks, movement keeps camera active, cooldown, dry-run and Stop");
}

void dashboardTextTests() {
 Request r{};reset();auto cb=open(r);char heartbeat[1025]{};
 uint8_t jpeg[20]{};uint32_t seq=0;
 assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);cb.videoSent(jpeg,4,seq,cb.context);
 assert(cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 assert(strstr(heartbeat,"dashboard_update_due=true") && !task.active);
 cb.event(gemini_live::Event::OutputText,"I can see another robot.",cb.context);
 const auto reply=state.liveResponseId;
 cb.event(gemini_live::Event::OutputText," I am watching it.",cb.context);
 assert(state.liveResponseId==reply && strstr(state.liveResponse,"I am watching it."));
 assert(!task.active && !mock::dispatched);
 cb.event(gemini_live::Event::TurnComplete,"",cb.context);
 mock::now+=5999;camera_service::capturedAt=mock::now;
 assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4);cb.videoSent(jpeg,4,seq,cb.context);
 assert(cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 assert(strstr(heartbeat,"dashboard_update_due=false"));
 ++mock::now;assert(cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 assert(strstr(heartbeat,"dashboard_update_due=true") && !task.active);
 taskStart(cb);const auto started=task.started, moves=task.moves;
 cb.event(gemini_live::Event::OutputText,"I will turn to inspect the opening.",cb.context);
 assert(state.liveResponseId==reply+1 && task.started==started && task.moves==moves);
 cb.event(gemini_live::Event::TurnComplete,"",cb.context);
 queue(cb,"turn",C::TurnLeft,20);
 assert(!cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context)); // Never interrupt a pending tool.
 robot_session::cancel();cb.event(gemini_live::Event::OutputText,"stale update",cb.context);
 assert(!strstr(state.liveResponse,"stale update") && !cb.readHeartbeat(heartbeat,sizeof(heartbeat),cb.context));
 puts("PASS: initial and six-second dashboard text requests, streamed replies, unchanged task budgets, tool-wait and Stop priority");
}

void freshConnectionTests() {
 for(unsigned phase=0;phase<4;++phase) {
  Request old{};reset();auto cb=open(old);
  if(phase==0)robot_session::cancel(); // An already-idle warm connection.
  if(phase==1 || phase==2) {
   taskStart(cb);queue(cb,"moving",C::Forward,10);
   if(phase==2)cb.event(gemini_live::Event::Error,"Gemini Live connection lost",cb.context);
  }
  if(phase==3) {
   assert(robot_session::sendText(mock::generation,"Explore"));
   int32_t stereo[320]{};robot_session::submit(stereo,160);
   assert(state.liveTextPending&&!mock::queued.empty());
  }
  const auto priorConnection=connection, priorCount=state.liveConnectionCount;
  {
   robot_actions::Guard control;
   robot_actions::stop("Dashboard STOP ALL button");
   robot_session::resetConnection();
  }
  assert(connection!=priorConnection && cb.cancelled(cb.context));
  assert(!state.liveActive && !state.liveConnected && state.liveConnecting);
  assert(!task.active && !reconnectPending && !queuedText[0] && !queuedRecovery[0]);
  assert(!state.liveTextPending && !toolResultReady && !pendingCommand && mock::queued.empty());
  assert(!mock::actionActive && !state.listening && !inputReady);
  cb.event(gemini_live::Event::Connected,"late old connection",cb.context);
  cb.event(gemini_live::Event::UserPrompt,"stale goal",cb.context);
  cb.event(gemini_live::Event::Error,"stale failure",cb.context);
  assert(!state.liveConnected && !task.active && !state.error[0]);
  assert(cb.action("stale-move",tool(C::Forward,5),cb.context)==gemini_live::ActionResult::Rejected);
  robot_session::poll();
  // Exercise the actual replacement-worker loop. No input resumes on setup.
  mock::liveHook=[&](const gemini_live::Callbacks& fresh,bool probe) {
   const auto& request=*static_cast<Request*>(fresh.context);
   assert(!probe && request.connection!=old.connection && !fresh.cancelled(fresh.context));
   assert(!fresh.mediaEpoch(fresh.context));
   fresh.event(gemini_live::Event::Connected,"Connected",fresh.context);
   uint8_t jpeg[20]{};uint32_t seq=0;int16_t pcm[320]{};char text[1025]{};
   assert(!fresh.readVideo(jpeg,sizeof(jpeg),&seq,fresh.context));
   assert(!fresh.readAudio(pcm,320,fresh.context));
   assert(!fresh.readText(text,sizeof(text),fresh.context));
   assert(!fresh.readRecovery(text,sizeof(text),fresh.context));
   assert(!fresh.readHeartbeat(text,sizeof(text),fresh.context));
   assert(fresh.action("unsolicited",tool(C::Forward,5),fresh.context)==gemini_live::ActionResult::Rejected);
  };
  try {worker(nullptr);assert(false);}catch(const mock::StopWorker&){}
  assert(mock::networkCalls==1 && state.liveConnected && !state.liveConnecting);
  assert(state.liveConnectionCount==priorCount+1 && !state.liveActive && !task.active);
  assert(!state.listening && !mock::actionActive);
  // Repeated Stop All during setup cancels that attempt too.
  Request next{connection,mock::configurationRevision,mock::now,0};
  robot_session::resetConnection();assert(cancelled(&next));
  event(gemini_live::Event::Connected,"stale setup",&next);
  assert(!state.liveConnected && state.liveConnecting);
 }
 reset();mock::settings.configured=false;robot_session::resetConnection();
 assert(!state.liveConnected && !state.liveConnecting && !state.liveActive);
 puts("PASS: Stop All replaces idle/active/recovering connections, cancels stale callbacks and queued input, and opens fresh setup without media or actions");
}

void microphoneQueueTests(){
 Request r{};reset();auto cb=open(r);int32_t stereo[320];
 for(unsigned i=0;i<160;++i){stereo[2*i]=1234*65536;stereo[2*i+1]=-999*65536;}
 robot_session::submit(stereo,160);int16_t pcm[640]{};
 assert(cb.readAudio(pcm,640,cb.context)==160&&pcm[0]==1234&&state.liveSentSamples==160);
 robot_session::submit(stereo,160);robot_session::cancel();assert(!cb.readAudio(pcm,640,cb.context)&&mock::queued.empty());
 reset();assert(robot_session::sendText(mock::generation,"Hello"));r={connection,mock::configurationRevision,mock::now,session};
 cb={&r,cancelled,readAudio,event,action,readVideo,videoSent,cancelTool,pollToolResult,canResume,mediaEpoch,taskActive,readText,readRecovery,readHeartbeat};
 cb.event(gemini_live::Event::Connected,"",cb.context);cb.event(gemini_live::Event::InputReady,"",cb.context);robot_session::poll();
 robot_session::submit(stereo,160);assert(!cb.readAudio(pcm,640,cb.context));
 robot_session::listen(mock::generation);assert(!state.liveTextOnly&&waitingForVoice);
 reset();robot_session::listen(mock::generation);r={connection,mock::configurationRevision,mock::now,session};
 cb.context=&r;mock::readyReactions=0;
 cb.event(gemini_live::Event::Connected,"",cb.context);cb.event(gemini_live::Event::InputReady,"",cb.context);robot_session::poll();
 assert(mock::readyReactions==1&&waitingForVoice&&!state.listening);
 robot_session::submit(stereo,160);assert(mock::queued.empty());
 mock::cuePlaying=mock::chimePlaying=mock::reactionActive=false;robot_session::poll();mock::now+=121;robot_session::poll();
 assert(!waitingForVoice&&state.listening);
 robot_session::submit(stereo,160);assert(cb.readAudio(pcm,640,cb.context)==160&&pcm[0]==1234);
 puts("PASS: processed microphone channel, queue freshness/session cancellation, text-only isolation and wake acknowledgment before speech upload");
}
void consoleTests(){
 Request r{};reset();auto cb=open(r);auto face=tool(C::Expression);strcpy(face.actionReason,"I noticed an unfamiliar object.");
 assert(cb.action("console-face",face,cb.context)==gemini_live::ActionResult::Queued);
 const auto id=action_console::snapshot().newestId;action_console::Entry e;
 assert(action_console::read(id,e)&&e.state==action_console::State::Queued);
 assert(!strcmp(e.purpose,face.actionReason));robot_session::poll();
 assert(action_console::read(id,e)&&e.state==action_console::State::Running);
 robot_session::poll();assert(action_console::read(id,e)&&e.state==action_console::State::Applied);
 assert(strstr(e.result,"face confirmed")&&strstr(e.result,"not confirmed"));
 result(cb,"console-face");
 assert(cb.action("console-cooldown",face,cb.context)==gemini_live::ActionResult::Queued);robot_session::poll();
 const auto blocked=action_console::snapshot().newestId;assert(action_console::read(blocked,e)&&e.state==action_console::State::Blocked);
 assert(strstr(e.result,"cooldown")&&!strcmp(e.purpose,face.actionReason));result(cb,"console-cooldown");
 auto observe=tool(C::Observe);assert(cb.action("console-cancel",observe,cb.context)==gemini_live::ActionResult::Queued);
 const auto cancelId=action_console::snapshot().newestId;robot_session::cancel();
 assert(action_console::read(cancelId,e)&&e.state==action_console::State::Cancelled);
 action_console::update(r.connection,"console-cancel",action_console::State::Completed,"late result");
 assert(action_console::read(cancelId,e)&&e.state==action_console::State::Cancelled);
 reset();cb=open(r,true);assert(cb.action("console-test",face,cb.context)==gemini_live::ActionResult::Queued);robot_session::poll();
 assert(action_console::read(action_console::snapshot().newestId,e)&&e.state==action_console::State::TestOnly);
 result(cb,"console-test");
 assert(cb.action("console-stop",tool(C::Stop),cb.context)==gemini_live::ActionResult::Queued);
 const auto stopId=action_console::snapshot().newestId;robot_session::poll();
 assert(action_console::read(stopId,e)&&e.state==action_console::State::Completed);
 const auto old=id;
 for(unsigned i=0;i<action_console::kCapacity+1;++i)action_console::local("example","","",action_console::State::Completed,"done");
 assert(!action_console::read(old,e));const auto snapshot=action_console::snapshot();
 assert(snapshot.newestId-snapshot.oldestId+1==action_console::kCapacity);
 assert(action_console::read(snapshot.newestId,e)&&!strcmp(e.function,"example"));
 action_console::updateId(old,action_console::State::Failed,"must not overwrite recycled slot");
 assert(action_console::read(snapshot.newestId,e)&&!strcmp(e.result,"done"));
 printf("PASS: console face acknowledgement, independent reasons, cooldown, cancellation, late result, test-only Stop and bounded history (%zu PSRAM bytes)\n",sizeof(action_console::Entry)*action_console::kCapacity);
}
int main(){microphoneQueueTests();toolSequencingTests();automaticTaskTests();recoveryTests();edgeTests();pushTests();connectionErrorTests();reconnectPreemptionTests();ambientTests();freshConnectionTests();dashboardTextTests();consoleTests();}
