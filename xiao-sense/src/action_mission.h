#pragma once
#include "robot_actions.h"
namespace action_mission {
bool start(robot_actions::Command command);
void cancel();
void poll();
bool active();
const char* phase();
}
