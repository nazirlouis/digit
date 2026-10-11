#include "gemini_live_mocks.h"
bool hasEvent(gemini_live::Event e){return std::find(events.begin(),events.end(),e)!=events.end();}
size_t messagesWith(const char* needle){
 size_t n=0;for(const auto& s:sentFrames)if(s.data.find(needle)!=std::string::npos)++n;return n;
}
int main(){
 auto cb=callbacks();
 resetMocks();
 connectionFrames={{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,true);
 assert(connects==1&&audioReads==0&&videoReads==0&&actionCalls==0&&playedSamples==0);
 assert(hasEvent(gemini_live::Event::Connected)&&logLevel==ESP_LOG_NONE&&activeAllocations==0);

 resetMocks();stopAt=4500;sendAdvanceMs=2;
 audioSource.assign(320,12);
 connectionFrames={{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(messagesWith("\"audio\":")>5&&messagesWith("audioStreamEnd")==0);
 assert(videoSends>=2&&videoReads==videoSends);
 uint32_t videoAt=0;
 for(const auto& s:sentFrames)if(s.data.find("\"video\":")!=std::string::npos){
   if(videoAt)assert(s.at-videoAt>=1000);videoAt=s.at;
 }
 assert(activeAllocations==0);

 resetMocks();audioSource.assign(640,1);maxAudioReads=4;stopAt=2200;
 connectionFrames={{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 size_t firstVideo=sentFrames.size();
 for(size_t i=0;i<sentFrames.size();++i)if(sentFrames[i].data.find("\"video\":")!=std::string::npos){firstVideo=i;break;}
 assert(firstVideo>=5&&messagesWith("\"audio\":")==4);
 assert(messagesWith("audioStreamEnd")==1);

 resetMocks();
 connectionFrames={{
   {"{\"setupComplete\":{}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_END\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"hello\"},\"outputTranscription\":{\"text\":\"hi\"},\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]},\"generationComplete\":true,\"turnComplete\":true}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"cancel-me\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"toolCallCancellation\":{\"ids\":[\"cancel-me\"]}}"}
 }};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(hasEvent(gemini_live::Event::UserActivity)&&hasEvent(gemini_live::Event::InputText)&&playedSamples==2);
 assert(hasEvent(gemini_live::Event::GenerationComplete)&&hasEvent(gemini_live::Event::TurnComplete));
 assert(actionCalls==1&&cancelledIds==std::vector<std::string>{"cancel-me"});
 assert(messagesWith("functionResponses")==0&&activeAllocations==0);

 resetMocks();
 connectionFrames={
   {{"{\"setupComplete\":{}}"},
    {"{\"sessionResumptionUpdate\":{\"resumable\":true,\"newHandle\":\"resume-safely\"}}"},
    {"{\"goAway\":{\"timeLeft\":\"20s\"}}"}},
   {{"{\"setupComplete\":{}}"}}
 };
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(connects==2&&messagesWith("\"handle\":\"resume-safely\"")==1&&actionCalls==0);

 resetMocks();
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"find a battery\"}}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_END\"}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"pending\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"sessionResumptionUpdate\":{\"resumable\":true,\"newHandle\":\"unsafe-with-tool\"}}"},
   {"{\"goAway\":{\"timeLeft\":\"0.1s\"}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(connects==1&&actionCalls==1&&hasEvent(gemini_live::Event::Error));
 assert(cancelledIds==std::vector<std::string>{"pending"}&&activeAllocations==0);

 resetMocks();
 const std::string first="{\"serverContent\":{\"inputTranscription\":";
 const std::string second="{\"text\":\"fragmented\"}}}";
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",9,true},{first,1,false},{second,0,true}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(hasEvent(gemini_live::Event::InputText)&&!hasEvent(gemini_live::Event::Error));
 assert(std::find(eventText.begin(),eventText.end(),"fragmented")!=eventText.end());

 resetMocks();
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"{\"serverContent\":{\"outputTranscription\":{\"text\":\"AQ.FAKE_PRIVATE_KEY_123456\"}}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(std::find(eventText.begin(),eventText.end(),"[credential omitted]")!=eventText.end());
 for(const auto& message:eventText)assert(message.find("AQ.FAKE_PRIVATE_KEY_123456")==std::string::npos);

 resetMocks();
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"camera-only\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"dance\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"spoken\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"fragment\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"second-in-utterance\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"}
 }};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(actionCalls==1&&messagesWith("rejected_no_action_started")==2);

 resetMocks();autoComplete=true;
 connectionFrames={
   {{"{\"setupComplete\":{}}"},
    {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
    {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"identify\"}}}"},
    {"{\"voiceActivity\":{\"type\":\"ACTIVITY_END\"}}"},
    {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"once\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
    {"{\"serverContent\":{\"turnComplete\":true}}"},
    {"{\"sessionResumptionUpdate\":{\"resumable\":true,\"newHandle\":\"after-action\"}}"},
    {"{\"goAway\":{\"timeLeft\":\"20s\"}}"}},
   {{"{\"setupComplete\":{}}"},
    {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"once\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"}}
 };
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",cb,false);
 assert(connects==2&&actionCalls==1&&messagesWith("mocked local outcome")==1);

 // A boot-time connection stays private: even unsolicited speech/VAD/tools
 // cannot read microphone/camera buffers or play anything before a wake.
 resetMocks();stopAt=13000;
 auto warm=callbacks();warm.mediaEpoch=getMediaEpoch;
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"old\"},\"outputTranscription\":{\"text\":\"old output\"},\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"idle-call\",\"name\":\"stop\",\"args\":{}}]}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&audioReads==0&&videoReads==0&&playedSamples==0&&actionCalls==0);
 assert(!hasEvent(gemini_live::Event::InputReady)&&!hasEvent(gemini_live::Event::UserActivity));
 assert(!hasEvent(gemini_live::Event::InputText)&&!hasEvent(gemini_live::Event::OutputText));
 assert(std::any_of(sentFrames.begin(),sentFrames.end(),[](const Sent& s){return (s.opcode&127)==9;}));

 // Stop/inactivity retains the same socket. A quick subsequent wake must
 // drain the prior response before capturing, then require NEW VAD+text.
 resetMocks();completeOnCancel=true;audioSource.assign(320,1);stopAt=3200;
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"",10,true,11},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"identify\"}}}"},
   {"{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"old-pending\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}",1,true,0},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"LATE_OLD_INPUT\"},\"outputTranscription\":{\"text\":\"LATE_OLD_OUTPUT\"},\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}",1,true,12},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"LATE_AFTER_WAKE\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"during-drain\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"serverContent\":{\"turnComplete\":true}}"},
   {"{\"serverContent\":{\"outputTranscription\":{\"text\":\"BEFORE_FRESH_EVIDENCE\"},\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"interimInputTranscription\":{\"text\":\"INTERIM_IGNORED\"}}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"new dance\"}}}"},
   {"{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"during-drain\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"new-call\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&playedSamples==4&&actionCalls==2);
 assert(messagesWith("clientContent")==1&&messagesWith("cancelled_conversation_ended")==1);
 assert(messagesWith("STALE_COMPLETION")==0);
 assert(std::count(events.begin(),events.end(),gemini_live::Event::InputReady)==2);
 for(const auto& value:eventText)assert(value.find("LATE_")==std::string::npos&&value.find("BEFORE_FRESH")==std::string::npos&&value.find("INTERIM_")==std::string::npos);
 for(const auto& s:sentFrames)if(s.data.find("\"audio\":")!=std::string::npos||s.data.find("\"video\":")!=std::string::npos)assert(s.epoch==11||s.epoch==12);

 // Failed authentication/network attempts retry with capped backoff, while
 // remaining microphone-free. No prior media, pending calls or IDs replay.
 resetMocks();connectFailures=2;stopAt=6000;
 connectionFrames={{},{},{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==3&&hasEvent(gemini_live::Event::Connected)&&audioReads==0&&videoReads==0);

 resetMocks();stopAt=2500;
 connectionFrames={{{"{\"setupComplete\":{}}"},
   {"{\"sessionResumptionUpdate\":{\"resumable\":true,\"newHandle\":\"idle-resume\"}}"},
   {"{\"goAway\":{\"timeLeft\":\"20s\"}}"}},{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==2&&messagesWith("\"handle\":\"idle-resume\"")==1&&audioReads==0&&videoReads==0);

 resetMocks();audioSource.assign(320,1);stopAt=9500;
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"hello\"}}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_END\"}}"},
   {"{\"serverContent\":{\"turnComplete\":true}}"},
   {"",10,true,0},{"",10,true,2},{"",10,true,0}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&!hasEvent(gemini_live::Event::Error)&&messagesWith("clientContent")==2);

 // Uploaded speech can precede server VAD. Its local energy evidence keeps
 // a rapid next wake muted until old activity/transcript/tool packets drain.
 resetMocks();audioSource.assign(320,900);stopAt=2500;
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"",10,true,0},{"",10,true,2},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"LATE_PRE_VAD_INPUT\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"pre-vad-old-call\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"serverContent\":{\"turnComplete\":true}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"identify now\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"pre-vad-new-call\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&actionCalls==1&&!hasEvent(gemini_live::Event::Error));
 assert(std::find(eventText.begin(),eventText.end(),"LATE_PRE_VAD_INPUT")==eventText.end());
 int16_t quiet[320]{}, transient[320]{}, speech[320];
 transient[100]=32767;std::fill_n(speech,320,int16_t(900));
 assert(!localSpeechEvidence(quiet,320)&&!localSpeechEvidence(transient,320));
 assert(!localSpeechEvidence(speech,100)&&localSpeechEvidence(speech,320));

 // A normal Stop racing a callback or base64 encoding omits obsolete media,
 // rather than confusing session cancellation with a socket failure.
 for(unsigned mode=0;mode<3;++mode){
   resetMocks();audioSource.assign(mode==2?0:320,1);stopAt=2100;
   stopDuringRead=mode==0;stopDuringPlayback=mode==1;stopDuringEncode=mode==2;
   connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
     {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
     {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"hello\"}}}"},
     {"{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
     {"{\"serverContent\":{\"turnComplete\":true}}"}}};
   gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
   assert(connects==1&&!hasEvent(gemini_live::Event::Error));
   if(mode==0||mode==2)assert(messagesWith("\"audio\":")==0&&messagesWith("\"video\":")==0);
 }
 resetMocks();audioSource.assign(320,1);stopAt=2100;stopDuringEncode=true;
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&!hasEvent(gemini_live::Event::Error)&&messagesWith("\"audio\":")==0);

 // Valid PCM/text may precede final input transcription. Hold it after new
 // VAD, then release it in order once final input provides fresh evidence.
 resetMocks();audioSource.assign(320,1);
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"outputTranscription\":{\"text\":\"early answer\"},\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]},\"generationComplete\":true,\"turnComplete\":true}}"},
   {"{\"serverContent\":{\"interimInputTranscription\":{\"text\":\"not final\"}}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"final request\"}}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&playedSamples==2&&hasEvent(gemini_live::Event::GenerationComplete)&&hasEvent(gemini_live::Event::TurnComplete));
 auto inputEvent=std::find(events.begin(),events.end(),gemini_live::Event::InputText);
 auto outputEvent=std::find(events.begin(),events.end(),gemini_live::Event::OutputText);
 assert(inputEvent<outputEvent&&std::count(eventText.begin(),eventText.end(),"early answer")==1);

 resetMocks();audioSource.assign(320,1);
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"},
   {"{\"serverContent\":{\"interrupted\":true}}"},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"new request\"}}}"},
   {"{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAABAA==\"}}]}}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==1&&playedSamples==2&&hasEvent(gemini_live::Event::Interrupted));

 resetMocks();audioSource.assign(320,1);clearEpochOnError=true;stopAt=3500;
 const std::string largeAudio="{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\""+std::string(64000,'A')+"\"}}]}}}";
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {largeAudio},{largeAudio},{largeAudio}},{{"{\"setupComplete\":{}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==2&&playedSamples==0&&hasEvent(gemini_live::Event::Error)&&activeAllocations==0);

 resetMocks();audioSource.assign(320,1);clearEpochOnError=true;completeOnCancel=true;stopAt=4500;
 connectionFrames={{{"{\"setupComplete\":{}}"},{"",10,true,1},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"identify\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"failure-pending\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"",8}},{{"{\"setupComplete\":{}}"},{"",10,true,2},
   {"{\"voiceActivity\":{\"type\":\"ACTIVITY_START\"}}"},
   {"{\"serverContent\":{\"inputTranscription\":{\"text\":\"identify again\"}}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"failure-pending\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"},
   {"{\"toolCall\":{\"functionCalls\":[{\"id\":\"new-after-failure\",\"name\":\"start_task\",\"args\":{\"goal\":\"Find an object\"}}]}}"}}};
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connects==2&&actionCalls==2&&messagesWith("STALE_COMPLETION")==0&&messagesWith("\"handle\"")==0);

 resetMocks();connectFailures=100;stopAt=100000;
 gemini_live::run("AQ.FAKE_PRIVATE_KEY_123456","gemini-3.8-live",warm,false);
 assert(connectAt.size()>=8);
 for(size_t i=1;i<connectAt.size();++i)assert(connectAt[i]-connectAt[i-1]<=30000);
 assert(connectAt.back()-connectAt[connectAt.size()-2]==30000);
 puts("PASS: complete mocked WSS lifecycle, probe privacy, audio-first/video cadence, send-clock regression, VAD/playback/cancel, safe GoAway resumption, fragmented frames, credential redaction");
 puts("PASS: persistent idle privacy/heartbeat, wake epochs on one socket, pause cancellation/drain, stale audio/tools/results quarantine, reconnect backoff and idle resumption");
 puts("PASS: completed/silent pause retains socket, callback/encoding Stop races, deferred output ordering, pending-tool network failure, capped 30-second retry");
 puts("PASS: deferred output cleared by interruption, PCM pre-transcript buffer limit fails closed without playback or leaked allocations");
 puts("PASS: Stop before server VAD drains old activity/transcript/tools; RMS/peak/sample-count evidence rejects silence and single transient");
}
