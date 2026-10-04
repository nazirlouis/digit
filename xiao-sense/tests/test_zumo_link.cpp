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
int main() {
  zumo_link::begin();
  reply(false);
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
  std::cout<<"PASS production UART bridge: default 6000, BRAKE support, older peer fallback, edge and lease priority\n";
}
