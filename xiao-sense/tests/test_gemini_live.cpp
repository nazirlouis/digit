#include "gemini_live_mocks.h"
#include "../src/live_audio.h"
bool tool(const char* json,Calls& calls){
 Json object{cJSON_Parse(json)};assert(object.value);auto cb=callbacks();
 return toolReply(nullptr,object.value,calls,cb);
}
int main(){
 resetMocks();
 assert(heartbeat(nullptr)&&sent=="digit"&&sentOpcode==(WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN));
 sendWorks=false;assert(!heartbeat(nullptr));sendWorks=true;
 auto cb=callbacks();assert(setup(nullptr,"gemini-3.8-live",cb,"safe-resume-handle"));
 Json config{cJSON_Parse(sent.c_str())};
 const auto* value=field(config.value,"setup");
 assert(!strcmp(text(field(value,"model")),"models/gemini-3.8-live"));
 assert(!strcmp(text(cJSON_GetArrayItem(field(field(value,"generationConfig"),"responseModalities"),0)),"AUDIO"));
 assert(field(value,"contextWindowCompression")&&field(value,"sessionResumption"));
 assert(!strcmp(text(field(field(value,"sessionResumption"),"handle")),"safe-resume-handle"));
 const auto* declarations=field(cJSON_GetArrayItem(field(value,"tools"),0),"functionDeclarations");
 assert(cJSON_GetArraySize(declarations)==9 && actionId("move_forward")==24 && actionId("find_battery")==0);
 assert(!strcmp(text(field(cJSON_GetArrayItem(declarations,2),"behavior")),"NON_BLOCKING"));
 assert(!setup(nullptr,"gemini-3.8-flash",cb));
 Calls calls;
 const char* valid=R"({"functionCalls":[{"id":"one","name":"start_task","args":{"goal":"Find the battery"}}]})";
 const size_t before=sentFrames.size();
 assert(tool(valid,calls)&&actionCalls==1&&calls.pending()&&sentFrames.size()==before);
 assert(tool(valid,calls)&&actionCalls==1);
 for(const char* invalid:{
   R"({"functionCalls":[{"id":"x1","name":"drive","args":{"distance_cm":5}}]})",
   R"({"functionCalls":[{"id":"x2","name":"move_forward","args":{"distance_cm":31}}]})",
   R"({"functionCalls":[{"id":"x3","name":"move_backward","args":{"distance_cm":11}}]})",
   R"({"functionCalls":[{"id":"x4","name":"turn_left","args":{"degrees":91}}]})",
   R"({"functionCalls":[{"id":"x5","name":"turn_right","args":{"degrees":0}}]})",
   R"({"functionCalls":[{"id":"x6","name":"move_forward","args":{"distance_cm":1.5}}]})",
   R"({"functionCalls":[{"id":"x7","name":"move_forward","args":{"distance_cm":1,"speed":6000}}]})",
   R"({"functionCalls":[{"id":"x8","name":"move_forward","args":{"distance_cm":1,"distance_cm":2}}]})",
   R"({"functionCalls":[{"id":"x9","name":"set_expression","args":{"expression":"evil","duration_ms":1000}}]})",
   R"({"functionCalls":[{"id":"x10","name":"set_expression","args":{"expression":"HAPPY","duration_ms":10001}}]})",
   R"({"functionCalls":[{"id":"x11","name":"finish_task","args":{"outcome":"maybe","reason":"done"}}]})",
   R"({"functionCalls":[{"id":"x12","name":"observe","args":{"move":true}}]})",
   R"({"functionCalls":[{"id":"x13","name":"run_action","args":{"action":"identify"}}]})"
 }) assert(tool(invalid,calls)&&actionCalls==1&&sent.find("rejected_no_action_started")!=std::string::npos);
 actionResult=gemini_live::ActionResult::TestOnly;
 assert(tool(R"({"functionCalls":[{"id":"two","name":"stop","args":{}}]})",calls));
 assert(actionCalls==2&&sent.find("test_only_no_action_started")!=std::string::npos);
 calls.cancel("one",cb); assert(!calls.pending()&&cancelledIds.size()==1);
 assert(tool(valid,calls)&&actionCalls==2);
 State state;
 actionResult=gemini_live::ActionResult::Queued;
 assert(tool(R"({"functionCalls":[{"id":"actual","name":"observe","args":{}}]})",state.calls));
 completions.push_back({"actual",R"({"result":"observed","front_valid":false,"front_mm":-1})"});
 assert(flushToolResults(nullptr,state,cb)&&!state.calls.pending());
 assert(sent.find("\"name\":\"observe\"")!=std::string::npos && sent.find("\"front_valid\":false")!=std::string::npos);
 assert(sent.find("\"scheduling\":\"INTERRUPT\"")!=std::string::npos);
 sendWorks=false; assert(!resultReply(nullptr,"failure","observe","rejected",cb)); sendWorks=true;
 stopped=true; assert(!tool(valid,state.calls)); stopped=false;
 using C=robot_actions::Command;
 State evidence;
 assert(!evidence.authorize(C::StartTask,false)&&evidence.authorize(C::Stop,false));
 evidence.startActivity(); assert(!evidence.authorize(C::StartTask,false));
 evidence.transcript(); assert(evidence.authorize(C::Expression,false)); assert(evidence.authorize(C::StartTask,false));
 evidence.transcript(); assert(!evidence.authorize(C::StartTask,false));
 assert(!evidence.authorize(C::Forward,false)&&evidence.authorize(C::Forward,true));
 mockMs+=kUserEvidenceMs+1; assert(evidence.authorize(C::TurnLeft,true)&&evidence.authorize(C::Observe,true));
 assert(evidence.authorize(C::FinishTask,true)&&!evidence.authorize(C::StartTask,false));
 evidence.epoch=0; assert(!evidence.authorize(C::TurnLeft,true));
 mockMs=1000;
 actionResult=gemini_live::ActionResult::TestOnly;
 const auto argumentlessBefore=actionCalls;
 assert(tool(R"({"functionCalls":[{"id":"noargs-observe","name":"observe"}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"noargs-stop","name":"stop"}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"noargs-move","name":"move_forward"}]})",calls));
 assert(actionCalls==argumentlessBefore+2 && sent.find("rejected_no_action_started")!=std::string::npos);
 assert(boundedJson("{\"ok\":true}",11));
 const std::string deep=std::string(13,'[')+"0"+std::string(13,']');
 assert(!boundedJson(deep.c_str(),deep.size())&&!boundedJson("{\"x\":\"\\u0000\"}",14));
 std::vector<int16_t> input(2400),all,split;
 for(size_t i=0;i<input.size();++i)input[i]=static_cast<int16_t>(i*143);
 live_audio::Resampler a,b;
 const auto append=[](std::vector<int16_t>& output){return [&output](const int16_t* p,size_t n){output.insert(output.end(),p,p+n);return true;};};
 assert(a.append(input.data(),input.size(),append(all)));
 for(size_t i=0;i<input.size();++i)assert(b.append(input.data()+i,1,append(split)));
 assert(all==split&&all.size()==1600);
 puts("PASS: Live setup/registry, async actual result, rejection/dry-run/cancel/dedup, writable heartbeat, bounded JSON, streaming resampling");
}
