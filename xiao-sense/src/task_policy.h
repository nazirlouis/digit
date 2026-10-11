#pragma once
#include <cstdint>
namespace robot_actions {
struct TaskPolicy {
  static constexpr uint32_t deadlineMs=90000;
  static constexpr unsigned maxMoves=12, maxObservations=24, maxNoProgress=3;
  bool active=false;
  uint32_t started=0;
  unsigned moves=0, observations=0, noProgress=0;
  void start(uint32_t now) { *this={}; active=true; started=now; }
  bool expired(uint32_t now) const { return active && now-started>=deadlineMs; }
  bool canMove() const { return active && moves<maxMoves && noProgress<maxNoProgress; }
  bool canObserve() const { return active && observations<maxObservations; }
  void moved(bool progress) { ++moves; noProgress=progress?0:noProgress+1; }
};
}
