#include "gemini_live_mocks.h"
#include <cmath>

bool tool(const char* json,Calls& calls,State* evidence=nullptr,bool allow=true){
 Json object{cJSON_Parse(json)};assert(object.value);auto cb=callbacks();
 return toolReply(nullptr,object.value,calls,cb,evidence,allow);
}
void actionReasonTests() {
 resetMocks(); Calls calls;
 assert(tool(R"({"functionCalls":[{"id":"face-reason","name":"set_expression","args":{"expression":"CURIOUS","duration_ms":4000,"action_reason":"I noticed an unfamiliar object."}}]})",calls));
 assert(actionCalls==1 && toolTraces.size()==2 && toolTraces[0].status=="Requested" && toolTraces[1].status=="Queued");
 assert(toolTraces[0].reason=="I noticed an unfamiliar object.");
 const auto before=toolTraces.size();
 assert(tool(R"({"functionCalls":[{"id":"face-reason","name":"set_expression","args":{"expression":"HAPPY","duration_ms":3000}}]})",calls));
 assert(actionCalls==1 && toolTraces.size()==before); // replay never rewrites its reason
 assert(tool(R"({"functionCalls":[{"id":"bad-reason","name":"observe","args":{"action_reason":42}}]})",calls));
 assert(actionCalls==1 && toolTraces.back().status=="Rejected");
 std::string longReason(192,'x');
 const auto json=std::string("{\"functionCalls\":[{\"id\":\"long-reason\",\"name\":\"observe\",\"args\":{\"action_reason\":\"")+longReason+"\"}}]}";
 assert(tool(json.c_str(),calls) && actionCalls==1 && toolTraces.back().status=="Rejected");
 assert(tool(R"({"functionCalls":[{"id":"stop-no-reason","name":"stop"}]})",calls));
 assert(actionCalls==2); // urgent Stop still accepts its original no-argument form
 assert(tool(R"({"functionCalls":[{"id":"ambient","name":"observe","args":{"action_reason":"Check the latest scene."}}]})",calls,nullptr,false));
 assert(actionCalls==2 && toolTraces.back().status=="Rejected");
 puts("PASS: per-call public explanations, legacy Stop, rejected metadata and deduplicated action trace");
}
void testBusyToolReplies() {
 resetMocks();
 State state; auto cb=callbacks();
 assert(tool(R"({"functionCalls":[{"id":"explore","name":"start_task","args":{"goal":"explore"}}]})",state.calls));
 assert(state.calls.pending()&&sentFrames.empty());
 actionResult=gemini_live::ActionResult::Busy;
 const char* early=R"({"functionCalls":[{"id":"early-observe","name":"observe","args":{}}]})";
 assert(tool(early,state.calls));
 Json busy{cJSON_Parse(sent.c_str())};
 const auto* reply=cJSON_GetArrayItem(field(field(busy.value,"toolResponse"),"functionResponses"),0);
 const auto* response=field(reply,"response");
 assert(!strcmp(text(field(reply,"id")),"early-observe"));
 assert(!strcmp(text(field(response,"result")),"busy_waiting_for_tool"));
 assert(cJSON_IsFalse(field(response,"action_started"))&&cJSON_IsTrue(field(response,"previous_tool_pending")));
 assert(!field(response,"scheduling"));
 assert(strstr(text(field(response,"instruction")),"Wait for the previous tool"));
 assert(state.calls.pending()&&state.calls.entries[state.calls.index("explore")].pending);
 assert(!state.calls.entries[state.calls.index("early-observe")].pending);
 const auto replies=sentFrames.size();
 assert(tool(early,state.calls)&&sentFrames.size()==replies); // duplicate never retries
 completions.push_back({"explore",R"({"result":"task_started","camera_sent":true,"task_active":true})"});
 assert(flushToolResults(nullptr,state,cb)&&!state.calls.pending());
 Json done{cJSON_Parse(sent.c_str())};
 reply=cJSON_GetArrayItem(field(field(done.value,"toolResponse"),"functionResponses"),0);
 assert(!strcmp(text(field(reply,"id")),"explore"));
 assert(!field(field(reply,"response"),"scheduling"));
 assert(cJSON_IsTrue(field(field(reply,"response"),"camera_sent")));
 // Busy is not a substitute for invalid/unauthorized rejection, and a real
 // Stop is still delivered immediately while the original tool is pending.
 actionResult=gemini_live::ActionResult::Rejected;
 assert(tool(R"({"functionCalls":[{"id":"rejected","name":"observe"}]})",state.calls));
 assert(sent.find("rejected_no_action_started")!=std::string::npos);
 actionResult=gemini_live::ActionResult::Queued;
 assert(tool(R"({"functionCalls":[{"id":"waiting","name":"observe"}]})",state.calls));
 const auto beforeStop=actionCalls;
 assert(tool(R"({"functionCalls":[{"id":"urgent-stop","name":"stop"}]})",state.calls));
 assert(actionCalls==beforeStop+1);
 puts("PASS: busy response is explicit and silent, actual completion resumes planning, dedup and Stop preserved");
}
int main(){
 actionReasonTests();
 testBusyToolReplies();
 resetMocks();
 assert(heartbeat(nullptr)&&sent=="digit"&&sentOpcode==(WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN));
 sendWorks=false;assert(!heartbeat(nullptr));sendWorks=true;
 auto cb=callbacks();assert(setup(nullptr,"gemini-robotics-er-2-streaming-preview",cb,"safe-resume-handle"));
 Json config{cJSON_Parse(sent.c_str())};
 const auto* value=field(config.value,"setup");
 assert(!strcmp(text(field(value,"model")),"models/gemini-robotics-er-2-streaming-preview"));
 assert(!strcmp(text(cJSON_GetArrayItem(field(field(value,"generationConfig"),"responseModalities"),0)),"TEXT"));
 assert(field(value,"contextWindowCompression")&&field(value,"sessionResumption"));
 assert(!strcmp(text(field(field(value,"sessionResumption"),"handle")),"safe-resume-handle"));
 const auto* defaultPart=cJSON_GetArrayItem(field(field(value,"systemInstruction"),"parts"),0);
 assert(!strcmp(text(field(defaultPart,"text")),gemini_live::kDefaultSystemInstructions));
 const auto* declarations=field(cJSON_GetArrayItem(field(value,"tools"),0),"functionDeclarations");
 assert(cJSON_GetArraySize(declarations)==11 && actionId("dance")==29 && actionId("push_object")==28 && actionId("move_forward")==24 && actionId("calibrate_surface")==0 && actionId("find_battery")==0);
 // Gemini 3.8 defaults to asynchronous tools. Every operation whose actual
 // outcome informs the next step must explicitly wait for that result.
 for(int i=0;i<cJSON_GetArraySize(declarations);++i) {
  const auto* declaration=cJSON_GetArrayItem(declarations,i);
  assert(!strcmp(text(field(declaration,"behavior")),"BLOCKING"));
  assert(field(field(field(declaration,"parameters"),"properties"),"action_reason"));
 }
 assert(!setup(nullptr,"gemini-3.8-flash",cb));
 const char* custom="## Personality and behavior\nBe friendly.\n\n## Base information\nYou are Digit.\n\n## Expression and emotions\nUse a playful voice with \"short\" answers. Emoji: \xf0\x9f\xa4\x96";
 assert(setup(nullptr,"gemini-robotics-er-2-streaming-preview",cb,"",custom));
 Json customConfig{cJSON_Parse(sent.c_str())};
 const auto* customPart=cJSON_GetArrayItem(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"),0);
 assert(!strcmp(text(field(customPart,"text")),custom));
 assert(cJSON_GetArraySize(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"))==5);
 const auto* welcomePart=cJSON_GetArrayItem(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"),3);
 assert(!strcmp(text(field(welcomePart,"text")),gemini_live::kNewChatContext));
 assert(gemini_live::validSystemInstructions(gemini_live::kNewChatContext));
 const auto* textPart=cJSON_GetArrayItem(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"),4);
 assert(!strcmp(text(field(textPart,"text")),gemini_live::kDashboardTextInstructions));
 assert(gemini_live::validSystemInstructions(gemini_live::kDashboardTextInstructions));
 assert(strstr(text(field(textPart,"text")),"normal response text"));
 assert(strstr(text(field(textPart,"text")),"dashboard_update_due=true"));
 assert(strstr(text(field(textPart,"text")),"actual tool result"));
 assert(strstr(text(field(textPart,"text")),"not private internal reasoning"));
 assert(!strstr(gemini_live::kNewChatContext,"Stay quiet when nothing meaningful changes"));
 const auto* policyPart=cJSON_GetArrayItem(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"),1);
 assert(strstr(text(field(policyPart,"text")),"50 movements per task") && strstr(text(field(policyPart,"text")),"dashboard typed message"));
 assert(!strstr(text(field(policyPart,"text")),"calibrat"));
 assert(strstr(text(field(policyPart,"text")),"automatically opens a local task"));
 assert(strstr(text(field(policyPart,"text")),"without resetting its limits"));
 assert(strstr(text(field(policyPart,"text")),"call finish_task after answering"));
 const auto* recoveryPart=cJSON_GetArrayItem(field(field(field(customConfig.value,"setup"),"systemInstruction"),"parts"),2);
 assert(strstr(text(field(recoveryPart,"text")),"Zumo owns bounded safety recovery") && strstr(text(field(recoveryPart,"text")),"push_object(distance_cm)"));
 const auto* navigation=text(field(recoveryPart,"text"));
 assert(strstr(navigation,"Call observe before the first movement")&&strstr(navigation,"busy_waiting_for_tool"));
 assert(strstr(navigation,"front sensor cannot see behind") && strstr(navigation,"no cumulative recovery-count limit"));
 assert(!strstr(navigation,"Up to three recoveries"));
 assert(strstr(navigation,"latest camera frame") && strstr(navigation,"figure out how to avoid it"));
 assert(strstr(navigation,"Choose your own route, turn angles and movement distances") && strstr(navigation,"continue the same task"));
 assert(!strstr(navigation,"3 to 5 cm steps") && !strstr(navigation,"small turn") && !strstr(navigation,"turn a small amount"));
 assert(strstr(navigation,"detour_pending") && strstr(navigation,"100 mm (10 cm)"));
 assert(strstr(navigation,"Turn to a different heading") && !strstr(navigation,"at most one edge retreat"));
 assert(strstr(navigation,"does not wait for IMU stability or further edge readings"));
 const std::string longInstructions(128*1024,'x');
 assert(setup(nullptr,"gemini-robotics-er-2-streaming-preview",cb,"",longInstructions.c_str()));
 Json longConfig{cJSON_Parse(sent.c_str())};
 const auto* longPart=cJSON_GetArrayItem(field(field(field(longConfig.value,"setup"),"systemInstruction"),"parts"),0);
 assert(text(field(longPart,"text"))==longInstructions);
 Calls calls;
 const char* valid=R"({"functionCalls":[{"id":"one","name":"start_task","args":{"goal":"Find the battery"}}]})";
 const size_t before=sentFrames.size();
 assert(tool(valid,calls)&&actionCalls==1&&calls.pending()&&sentFrames.size()==before);
 assert(tool(valid,calls)&&actionCalls==1);
 for(const char* invalid:{
   R"({"functionCalls":[{"id":"removed-cal","name":"calibrate_surface","args":{}}]})",
   R"({"functionCalls":[{"id":"removed-cal-noargs","name":"calibrate_surface"}]})",
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
   ,R"({"functionCalls":[{"id":"p1","name":"push_object","args":{"distance_cm":0}}]})"
   ,R"({"functionCalls":[{"id":"p2","name":"push_object","args":{"distance_cm":-5}}]})"
   ,R"({"functionCalls":[{"id":"p3","name":"push_object","args":{"distance_cm":31}}]})"
   ,R"({"functionCalls":[{"id":"p4","name":"push_object","args":{"distance_cm":1.5}}]})"
   ,R"({"functionCalls":[{"id":"p5","name":"push_object","args":{"distance_cm":5,"speed":6000}}]})"
   ,R"({"functionCalls":[{"id":"p6","name":"push_object","args":{"distance_cm":5,"direction":"backward"}}]})"
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
 assert(sent.find("scheduling")==std::string::npos);
 sendWorks=false; assert(!resultReply(nullptr,"failure","observe","rejected",cb)); sendWorks=true;
 stopped=true; assert(!tool(valid,state.calls)); stopped=false;
 using C=robot_actions::Command;
 robot_actions::ToolRequest pushRequest;
 Json pushArgs{cJSON_Parse("{\"distance_cm\":30}")};
 assert(parseTool("push_object",pushArgs.value,pushRequest) && pushRequest.command==C::Push && pushRequest.amount==30);
 State evidence;
 assert(!evidence.authorize(C::Forward,true));
 evidence.inputSent=true;
 assert(evidence.authorize(C::Expression,false)&&evidence.authorize(C::Observe,false));
 assert(!evidence.authorize(C::StartTask,false)&&!evidence.authorize(C::Forward,false));
 assert(evidence.authorize(C::Forward,true)&&evidence.authorize(C::Push,true));
 evidence.epoch=0;assert(!evidence.authorize(C::Stop,false));
 mockMs=1000;
 actionResult=gemini_live::ActionResult::TestOnly;
 const auto argumentlessBefore=actionCalls;
 assert(tool(R"({"functionCalls":[{"id":"noargs-dance","name":"dance"}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"bad-dance","name":"dance","args":{"duration_ms":30000}}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"noargs-observe","name":"observe"}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"noargs-stop","name":"stop"}]})",calls));
 assert(tool(R"({"functionCalls":[{"id":"noargs-move","name":"move_forward"}]})",calls));
 assert(actionCalls==argumentlessBefore+3 && sent.find("rejected_no_action_started")!=std::string::npos);
 assert(boundedJson("{\"ok\":true}",11));
 const std::string deep=std::string(13,'[')+"0"+std::string(13,']');
 assert(!boundedJson(deep.c_str(),deep.size())&&!boundedJson("{\"x\":\"\\u0000\"}",14));
 puts("PASS: Live setup/registry, async actual result, rejection/dry-run/cancel/dedup, writable heartbeat, bounded JSON, ER 2 text-only policy");
}
