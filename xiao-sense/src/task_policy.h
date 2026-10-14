#pragma once
#include <cstdint>
namespace robot_actions {
struct TaskPolicy {
  static constexpr uint32_t deadlineMs=90000;
  static constexpr unsigned maxMoves=50, maxObservations=24, maxNoProgress=3;
  static constexpr unsigned recoveryMaxCm=5, maxDetourMoves=6;
  bool active=false;
  bool hasObservation=false;
  bool recoveryRequired=false, recoveryInFlight=false;
  bool detourPending=false;
  bool headingRequired=false;
  unsigned recoveryLimitCm=recoveryMaxCm;
  uint32_t started=0;
  unsigned moves=0, observations=0, noProgress=0, recoveries=0, edgeRetreats=0, detourMoves=0;
  void start(uint32_t now) { *this={}; active=true; started=now; }
  bool expired(uint32_t now) const { return active && now-started>=deadlineMs; }
  bool canMove() const { return active && moves<maxMoves && noProgress<maxNoProgress && detourMoves<maxDetourMoves; }
  bool canObserve() const { return active && observations<maxObservations; }
  bool canRetreatAtEdge() const {
    return canMove() && moves+2<=maxMoves &&
        !recoveryRequired && !recoveryInFlight;
  }
  bool edgeRetreated() {
    if (!canRetreatAtEdge()) return false;
    ++recoveries; ++edgeRetreats; detourPending=true;
    moved(false); // Interrupted forward move.
    ++moves;     // Local retreat consumes a move, not goal progress.
    return noProgress<maxNoProgress;
  }
  void moved(bool progress) { ++moves; noProgress=progress?0:noProgress+1; }
  // Count firmware attempts for diagnostics, without a cumulative recovery cap.
  // Recoveries cannot reset task time, movement, edge, or progress limits.
  bool localRecovery(unsigned attempts, bool edge=false, bool retreated=false) {
    recoveries += attempts;
    if (edge) ++edgeRetreats;
    if (retreated) ++moves;
    return moves<maxMoves;
  }
  void blocked() { moved(false); detourPending=headingRequired=true; }
  // Measured turns/reverses can prepare a detour, but cannot clear an
  // obstruction or keep resetting the lack-of-progress budget on their own.
  void completedMove(bool progress, bool forwardProgress) {
    if (!detourPending) { moved(progress); return; }
    ++moves; ++detourMoves;
    if (forwardProgress) { detourPending=false; detourMoves=0; noProgress=0; }
    else if (!progress) ++noProgress;
  }
  bool offerRecovery(bool edge=false) {
    moved(false); // Failed travel counts toward the original task's limits.
    if (!canMove() || recoveryInFlight) return false;
    if (edge) ++edgeRetreats;
    recoveryLimitCm=edge ? 10 : recoveryMaxCm;
    ++recoveries; recoveryRequired=true; detourPending=true; return true;
  }
};
}
