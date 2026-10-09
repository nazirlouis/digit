#pragma once
#include "robot_actions.h"
namespace action_mission {
bool start(robot_actions::Command command, int selectedSpeed = 4000);
void cancel();
void poll();
bool active();
const char* phase();
}
