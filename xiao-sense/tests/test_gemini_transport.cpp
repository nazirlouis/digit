#include "gemini_live_mocks.h"
bool hasEvent(gemini_live::Event e){return std::find(events.begin(),events.end(),e)!=events.end();}
size_t messagesWith(const char* needle){size_t n=0;for(const auto& s:sentFrames)if(s.data.find(needle)!=std::string::npos)++n;return n;}
void connectionReliability() {
 auto cb=callbacks();
 resetMocks();maxFrameWrite=4096;
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 const std::string instructions="Digit instructions: "+std::string(48000,'x')+" \xf0\x9f\xa4\x96";
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",cb,true,instructions.c_str());
 assert(hasEvent(gemini_live::Event::Connected)&&sentFrames.size()==1&&wireFrames.size()>20);
 for(size_t i=0;i<wireFrames.size();++i) {
  assert(wireFrames[i].data.size()<=2048);
  assert((wireFrames[i].opcode&15)==(i?WS_TRANSPORT_OPCODES_CONT:WS_TRANSPORT_OPCODES_TEXT));
  assert(bool(wireFrames[i].opcode&WS_TRANSPORT_OPCODES_FIN)==(i+1==wireFrames.size()));
 }
 Json setupMessage{cJSON_Parse(sentFrames.front().data.c_str())};
 const auto* part=cJSON_GetArrayItem(field(field(field(setupMessage.value,"setup"),"systemInstruction"),"parts"),0);
 assert(!strcmp(text(field(part,"text")),instructions.c_str()));
 assert(audioReads==0&&videoReads==0&&actionCalls==0&&activeAllocations==0);
 for(int failure=0;failure<3;++failure) {
  resetMocks();stopAt=30000;
  if(failure==0)failWriteAt=2;
  if(failure==1)stopAfterWrite=2;
  if(failure==2)sendAdvanceMs=5000;
  connectionFrames={{{R"({"setupComplete":{}})"}}};
  gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",cb,true,instructions.c_str());
  assert(wireFrames.size()==2&&sentFrames.empty()&&!hasEvent(gemini_live::Event::Connected));
  assert(!audioReads&&!videoReads&&!actionCalls&&activeAllocations==0);
 }
 resetMocks();
 std::string mediaPayload(6000,'x');currentMediaEpoch=4;clearEpochAfterWrite=1;cb.mediaEpoch=getMediaEpoch;
  WriteFailure endedMedia;
  assert(!send(reinterpret_cast<void*>(2),mediaPayload.data(),cb,4,1500,&endedMedia));
 assert(wireFrames.size()==1&&sentFrames.empty());
  assert(!strcmp(endedMedia.reason,"media session ended during write") && endedMedia.offset==2048 && endedMedia.expected==6000);
 cb.mediaEpoch=nullptr;
 resetMocks();failWriteAt=2;
 std::vector<uint8_t> image(6000,0x5a);std::vector<char> outgoing(9000);
 WriteFailure mediaFailure;
 assert(!media(reinterpret_cast<void*>(2),outgoing.data(),outgoing.size(),"video","image/jpeg",
     image.data(),image.size(),cb,0,&mediaFailure));
 assert(!strcmp(mediaFailure.reason,"transport write")&&mediaFailure.offset==2048&&mediaFailure.expected==2048);
 assert(sentFrames.empty()); // A partly written image cannot become an uploaded preview.
 resetMocks();connectFailures=3;stopAt=12000;
 auto warm=callbacks();warm.mediaEpoch=getMediaEpoch;
 connectionFrames.resize(5);
 connectionFrames[3]={{R"({"setupComplete":{}})"},{"",WS_TRANSPORT_OPCODES_CLOSE}};
 connectionFrames[4]={{R"({"setupComplete":{}})"}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",warm,false);
 assert(connects==5&&connectAt[4]-connectAt[3]>=1000&&connectAt[4]-connectAt[3]<1100);
 assert(!audioReads&&!videoReads&&!actionCalls&&activeAllocations==0);
 for(int failure=0;failure<3;++failure) {
  resetMocks();
  if(failure==0){connectFailures=1;upgradeStatus=429;}
  if(failure==1)connectionFrames={{{R"({"error":{"code":403,"message":"AQ.FAKE_PRIVATE_KEY_123456"}})"}}};
  if(failure==2)connectionFrames={{{std::string("\x03\xef",2)+"private server detail",WS_TRANSPORT_OPCODES_CLOSE}}};
  gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",cb,true);
  const auto expected=failure==0?"rate limited":failure==1?"API key refused":"1007";
  assert(hasEvent(gemini_live::Event::Error));
  assert(std::any_of(eventText.begin(),eventText.end(),[&](const std::string& value){return value.find(expected)!=std::string::npos;}));
  for(const auto& value:eventText)assert(value.find("AQ.FAKE_PRIVATE_KEY_123456")==std::string::npos&&value.find("private server detail")==std::string::npos);
 }
 puts("PASS: small WSS fragments preserve long instructions; short-write/Stop/deadline cancel safely; healthy setup resets backoff; precise redacted errors");
}


void microphoneInputTests(){
 resetMocks();currentMediaEpoch=21;stopAt=4500;auto cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 cb.readAudio=[](int16_t* out,size_t capacity,void*)->size_t{
  ++audioReads;if(audioReads!=1)return 0;assert(capacity>=320);std::fill_n(out,320,int16_t(1000));
  frames.push_back({R"({"voiceActivity":{"type":"ACTIVITY_START"}})"});
  frames.push_back({R"({"serverContent":{"inputTranscription":{"text":"Explore"}}})"});
  frames.push_back({R"({"serverContent":{"inputTranscription":{"text":" the room"}}})"});
  frames.push_back({R"({"voiceActivity":{"type":"ACTIVITY_END"}})"});
  frames.push_back({R"({"toolCall":{"functionCalls":[{"id":"spoken-move","name":"move_forward","args":{"distance_cm":5}}]}})"});return 320;
 };
 cb.readHeartbeat=[](char*,size_t,void*)->size_t{assert(false);return 0;};
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",cb,false);
 assert(std::count(events.begin(),events.end(),gemini_live::Event::UserPrompt)==1);
 assert(messagesWith("audio/pcm;rate=16000")==1&&actionCalls==1&&mockTaskActive);
 assert(videoSends>=3&&playedSamples==0&&activeAllocations==0);
 // ER can expose transcription without separate voiceActivity events. Fresh
 // locally transmitted speech still opens exactly one task, never a fragment.
 resetMocks();currentMediaEpoch=22;stopAt=2600;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 cb.readAudio=[](int16_t* out,size_t,void*)->size_t{
  ++audioReads;if(audioReads!=1)return 0;std::fill_n(out,320,int16_t(1000));
  frames.push_back({R"({"serverContent":{"inputTranscription":{"text":"Hello"}}})"});
  frames.push_back({R"({"serverContent":{"inputTranscription":{"text":" Digit"}}})"});
  frames.push_back({R"({"serverContent":{"modelTurn":{"parts":[{"text":"A friendly greeting."}]},"turnComplete":true}})"});return 320;
 };
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456","gemini-robotics-er-2-streaming-preview",cb,false);
 assert(std::count(events.begin(),events.end(),gemini_live::Event::UserPrompt)==1&&playedSamples==0&&activeAllocations==0);
 puts("PASS: raw 16 kHz microphone input, spoken task authority once per utterance, no heartbeat during speech/tool, concurrent video and no spoken output");
}
int main(){
 connectionReliability();
 microphoneInputTests();
 constexpr const char* model="gemini-robotics-er-2-streaming-preview";
 resetMocks();currentMediaEpoch=11;stopAt=6500;
 auto cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
 assert(videoSends>=5&&audioReads==0&&playedSamples==0&&actionCalls==0);
 uint32_t previous=0;for(const auto& packet:sentFrames)if(packet.data.find("\"video\"")!=std::string::npos){if(previous)assert(packet.at-previous>=1000);previous=packet.at;}
 assert(activeAllocations==0);
 // Real JPEG writes take time. Keep one-second upload starts rather than
 // adding that write time to every camera interval.
 resetMocks();currentMediaEpoch=31;stopAt=6500;sendAdvanceMs=15;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 jpeg.assign(15000,0);jpeg[0]=0xff;jpeg[1]=0xd8;jpeg[jpeg.size()-2]=0xff;jpeg.back()=0xd9;
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
 previous=0;unsigned largeFrames=0;
 for(const auto& packet:sentFrames)if(packet.data.find("\"video\"")!=std::string::npos){
  if(previous)assert(packet.at-previous>=1000&&packet.at-previous<=1010);previous=packet.at;++largeFrames;
 }
 assert(largeFrames>=5&&activeAllocations==0);
 // Ambient prompts authorize only reactions and do not emit user requests.
 // While a blocking tool is pending, video continues and no heartbeat repeats.
 resetMocks();currentMediaEpoch=12;stopAt=6500;unsigned prompts=0;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 cb.readHeartbeat=[](char* output,size_t capacity,void*)->size_t{
  const char* text="[CONTROLLER OBSERVATION]";snprintf(output,capacity,"%s",text);
  frames.push_back({R"({"toolCall":{"functionCalls":[{"id":"face","name":"set_expression","args":{"expression":"CURIOUS","duration_ms":3000}}]}})"});return strlen(text);
 };
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
 prompts=unsigned(messagesWith("[CONTROLLER OBSERVATION]"));
 assert(prompts==1&&videoSends>=5&&actionCalls==1);
 assert(std::count(events.begin(),events.end(),gemini_live::Event::HeartbeatSent)==1);
 assert(!hasEvent(gemini_live::Event::UserPrompt)&&!mockTaskActive);
 assert(!audioReads&&!playedSamples&&activeAllocations==0);
 // Native text parts are read directly; thoughts and inline audio are ignored.
 resetMocks();currentMediaEpoch=13;stopAt=2600;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 bool typed=false;
 cb.readText=[](char* out,size_t cap,void*)->size_t{
  static bool delivered=false;if(delivered)return 0;delivered=true;snprintf(out,cap,"Explore");
  frames.push_back({R"({"serverContent":{"modelTurn":{"parts":[{"text":"private thought","thought":true},{"text":"I see an object."},{"inlineData":{"mimeType":"audio/pcm;rate=24000","data":"AAABAA=="}}]},"turnComplete":true}})"});
  frames.push_back({R"({"toolCall":{"functionCalls":[{"id":"move","name":"move_forward","args":{"distance_cm":5}}]}})"});return 7;
 };
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
 typed=hasEvent(gemini_live::Event::UserPrompt);assert(typed&&mockTaskActive&&actionCalls==1);
 assert(std::count(events.begin(),events.end(),gemini_live::Event::OutputText)==1);
 assert(std::find(eventText.begin(),eventText.end(),"I see an object.")!=eventText.end());
 assert(std::find(eventText.begin(),eventText.end(),"private thought")==eventText.end());
 assert(!audioReads&&!playedSamples&&activeAllocations==0);
 // Stop during encoding discards the image and retains no uploaded preview.
 resetMocks();currentMediaEpoch=14;stopDuringEncode=true;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
 connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
 assert(videoSends==0&&messagesWith("\"video\"")==0&&activeAllocations==0);
  // Ending a conversation mid-image closes its partial write but must not
  // overwrite the controlling edge/Stop outcome with an encoding error.
  resetMocks();currentMediaEpoch=14;clearEpochAfterWrite=2;cb=callbacks();cb.mediaEpoch=getMediaEpoch;
  jpeg.assign(15000,0);jpeg[0]=0xff;jpeg[1]=0xd8;jpeg[jpeg.size()-2]=0xff;jpeg.back()=0xd9;
  connectionFrames={{{R"({"setupComplete":{}})"}}};
  gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,false);
  assert(videoSends==0&&messagesWith("\"video\"")==0&&activeAllocations==0);
  assert(std::none_of(eventText.begin(),eventText.end(),[](const std::string& text){return text.find("Live camera upload failed")!=std::string::npos;}));
 // Test connection reads no camera and calls no action even if frames arrive.
 resetMocks();cb=callbacks();connectionFrames={{{R"({"setupComplete":{}})"}}};
 gemini_live::run("FAKE_PRIVATE_KEY_123456",model,cb,true);
 assert(hasEvent(gemini_live::Event::Connected)&&!videoReads&&!audioReads&&!actionCalls&&activeAllocations==0);
 puts("PASS: 1 FPS limit, continuous frames during pending tools, idle-only heartbeat, typed task authority, text parsing, Stop and action-free connection probe");
}
