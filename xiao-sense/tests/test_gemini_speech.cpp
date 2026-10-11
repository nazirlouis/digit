// Actual speech lifecycle + queued tools; network and hardware are mocked.
#include "speech_commands.cpp"
using C=robot_actions::Command;
void reset() {
  free(outputPcm); free(mock::queueStorage); state={}; task={};
  outputPcm=nullptr; outputLock=nullptr; inputQueue=nullptr; mock::queueStorage=nullptr; mock::queued.clear();
  outputHead=outputCount=outputSession=outputEpoch=0; playbackEpoch=1; resampler={};
  session=connection=1; connectionRevision=0; generation=opensAt=activationAt=safetyEvents=idleAt=offeredVideoAt=responseProgressAt=0; idleMs=30000;
  waitingForVoice=awaitingReadyReaction=workerBusy=wasConnected=inputReady=false;
  ownedAction=stopOwned=toolResultReady=dispatching=dispatchCancelled=responsePending=idleArmed=toolStopPending=false;
  observationPending=observationSent=expressionPending=stopExpression=false;
  ownedGeneration=ownedSession=pendingSession=pendingGeneration=pendingAt=0;
  pendingCommand=0; pendingCall[0]=ownedCall[0]=toolResultCall[0]=0; newInput=newOutput=true;
  mock::now=1000; mock::generation=10; mock::safetyEvents=4; mock::configurationRevision=1;
  mock::connected=mock::actionAvailable=true; mock::actionActive=false;
  mock::queueFull=mock::semaphoreBusy=mock::taskCreationFails=false;
  mock::playing=mock::cuePlaying=mock::chimePlaying=mock::reactionActive=false;
  mock::settings={true,"gemini-test-live",false,30};
  mock::actionAttempts=mock::dispatched=mock::stops=0;
  mock::expressionCancels=0;
  mock::criticalDepth=mock::controlDepth=mock::mutexDepth=0;
  mock::beforeStartHook=mock::afterCriticalExit=mock::afterMutexGive=nullptr;
  camera_service::available=true; camera_service::capturedAt=mock::now; camera_service::copies=0;
  robot_actions::outcome=robot_actions::Result::Running;
  speech_commands::begin(); assert(state.ready);
}
gemini_live::Callbacks open(Request& r, bool dry=false) {
  assert(speech_commands::startLive(mock::generation,false,dry));
  r={connection,mock::configurationRevision,mock::now,session};
  gemini_live::Callbacks cb{&r,cancelled,readAudio,playAudio,event,action,readVideo,videoSent,cancelTool,pollToolResult,canResume,mediaEpoch,taskActive};
  cb.event(gemini_live::Event::Connected,"",cb.context); cb.event(gemini_live::Event::InputReady,"",cb.context); speech_commands::poll(); return cb;
}
robot_actions::ToolRequest tool(C c, int amount=0) { robot_actions::ToolRequest r; r.command=c; r.amount=amount; strcpy(r.goal,"Find the battery"); strcpy(r.expression,"HAPPY"); return r; }
void queue(const gemini_live::Callbacks& cb,const char* id,C c,int amount=0) { assert(cb.action(id,tool(c,amount),cb.context)==gemini_live::ActionResult::Queued); speech_commands::poll(); }
void observation(const gemini_live::Callbacks& cb) {
  uint8_t jpeg[20]; uint32_t seq=0;
  assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==0); // pre-request frame cannot answer.
  mock::now+=100; camera_service::capturedAt=mock::now;
  assert(cb.readVideo(jpeg,sizeof(jpeg),&seq,cb.context)==4 && !toolResultReady);
  cb.videoSent(4,seq,cb.context); speech_commands::poll(); assert(toolResultReady);
}
std::string result(const gemini_live::Callbacks& cb, const char* expected) {
  char id[129]{},text[768]{}; assert(cb.pollToolResult(id,sizeof(id),text,sizeof(text),cb.context)); assert(!strcmp(id,expected)); return text;
}
void taskStart(const gemini_live::Callbacks& cb) { queue(cb,"start",C::StartTask); assert(task.active && observationPending); observation(cb); assert(result(cb,"start").find("task_started")!=std::string::npos); }
int main() {
  static_assert(sizeof(speech_commands::Activity)<=64,"Keep real-time snapshots small");
  Request r{};
  reset(); auto cb=open(r);
  assert(!cb.taskActive(cb.context)); uint8_t jpg[20]; uint32_t seq; assert(!cb.readVideo(jpg,sizeof(jpg),&seq,cb.context));
  cb.event(gemini_live::Event::InputText,"hi",cb.context); cb.event(gemini_live::Event::OutputText,"Hi!",cb.context); cb.event(gemini_live::Event::TurnComplete,"",cb.context);
  speech_commands::poll(); mock::now+=30001; speech_commands::poll();
  assert(!state.liveActive && state.liveConnected && !task.active && !mock::dispatched);
  // No task means no movement, even with local hardware ready.
  reset(); cb=open(r); queue(cb,"unsolicited",C::Forward,10); assert(!mock::dispatched && !task.active); result(cb,"unsolicited");
  // Actual tool result is delayed until rest AND a frame captured after rest.
  reset(); cb=open(r); taskStart(cb); queue(cb,"move",C::Forward,10);
  assert(mock::dispatched==1 && ownedAction && task.active && !toolResultReady);
  cb.event(gemini_live::Event::TurnComplete,"",cb.context); mock::now+=30001; speech_commands::poll(); assert(task.active && state.liveActive);
  // Faces can be changed independently during motion and do not cancel it.
  queue(cb,"face",C::Expression); speech_commands::poll(); assert(result(cb,"face").find("visible")!=std::string::npos && ownedAction && mock::actionActive);
  mock::actionActive=false; robot_actions::outcome=robot_actions::Result::Completed; speech_commands::poll();
  assert(!ownedAction && observationPending && !toolResultReady && task.moves==1); observation(cb);
  const auto answer=result(cb,"move"); assert(answer.find("\"travelled_mm\":100")!=std::string::npos && answer.find("\"camera_sent\":true")!=std::string::npos);
  queue(cb,"second",C::TurnLeft,30); assert(mock::dispatched==2); // no new speech required.
  cb.cancelTool("second",cb.context); assert(!state.liveActive && !task.active); speech_commands::poll(); assert(mock::stops==1 && !mock::actionActive && state.liveConnected);
  // Dry-run starts a bounded virtual task but never calls the motion/face dispatchers.
  reset(); cb=open(r,true); taskStart(cb); queue(cb,"dry",C::Forward,10); observation(cb);
  assert(result(cb,"dry").find("test_only_no_movement")!=std::string::npos && !mock::dispatched);
  // Finish is explicit and distinct from turnComplete; deadline cannot be refreshed by tool results.
  reset(); cb=open(r); taskStart(cb); cb.event(gemini_live::Event::TurnComplete,"",cb.context); speech_commands::poll(); assert(task.active);
  auto finish=tool(C::FinishTask); strcpy(finish.outcome,"completed"); strcpy(finish.reason,"Found it");
  assert(cb.action("finish",finish,cb.context)==gemini_live::ActionResult::Queued); speech_commands::poll(); assert(!task.active); result(cb,"finish");
  reset(); cb=open(r); taskStart(cb); mock::now=task.started+90000; speech_commands::poll(); assert(!state.liveActive && !task.active);
  // Missing post-request image terminates task, rather than silently planning from stale imagery.
  reset(); cb=open(r); queue(cb,"start",C::StartTask); camera_service::available=false; mock::now+=3501; speech_commands::poll(); assert(!task.active); result(cb,"start");
  // Stop invalidates queued observations and an in-flight dispatch; warm socket remains.
  reset(); cb=open(r); queue(cb,"start",C::StartTask); speech_commands::cancel(); assert(!observationPending && !task.active && state.liveConnected);
  assert(!cb.readVideo(jpg,sizeof(jpg),&seq,cb.context));
  reset(); cb=open(r); taskStart(cb); mock::beforeStartHook=[] { speech_commands::cancel(); }; queue(cb,"race",C::Forward,10); speech_commands::poll(); assert(!mock::actionActive && mock::stops==1);
  reset(); cb=open(r); mock::beforeStartHook=[] { speech_commands::cancel(); }; queue(cb,"face-race",C::Expression); speech_commands::poll(); assert(!expressionPending && mock::expressionCancels==1);
  reset(); cb=open(r); taskStart(cb); queue(cb,"queued-result",C::Observe); observation(cb); assert(toolResultReady);
  assert(cb.action("priority-stop",tool(C::Stop),cb.context)==gemini_live::ActionResult::Queued);
  assert(!state.liveActive && !task.active && !toolResultReady); speech_commands::poll(); assert(mock::stops==1 && state.liveConnected);
  // Task bounds, including millisecond wrap.
  robot_actions::TaskPolicy p; p.start(UINT32_MAX-100); assert(!p.expired(100)); assert(p.expired(90000));
  p.start(0); for(unsigned i=0;i<12;++i)p.moved(true); assert(!p.canMove()); p.start(0); for(unsigned i=0;i<3;++i)p.moved(false); assert(!p.canMove());
  puts("PASS: greeting idle, task continuation, confirmed result plus fresh image, independent silent faces, Stop/races, dry-run, explicit finish, deadline and progress bounds");
}
