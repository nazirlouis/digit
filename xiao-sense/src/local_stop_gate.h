#pragma once
#include <cstdint>

namespace local_stop {
// Called under the module's short critical section. Keeping this coordination
// separate from inference lets host tests exercise races using the real gate.
struct Tag {
  uint32_t epoch = 0;
  uint32_t capturedAt = 0;
};
class Gate {
 public:
  static constexpr uint32_t kFreshMs = 650;
  bool update(bool active, bool manual, uint32_t generation) {
    const bool changed = active != active_ || manual != manual_ ||
                         (!manual && generation != generation_);
    if (changed) invalidate();
    active_ = active;
    manual_ = manual;
    generation_ = generation;
    return changed;
  }
  void invalidate() { ++epoch_; }
  Tag tag(uint32_t now) const { return {epoch_, now}; }
  bool accepts(Tag tag, uint32_t now) const {
    return active_ && tag.epoch == epoch_ && now - tag.capturedAt <= kFreshMs;
  }
  bool active() const { return active_; }
  bool manual() const { return manual_; }
  uint32_t generation() const { return generation_; }
 private:
  uint32_t epoch_ = 1, generation_ = 0;
  bool active_ = false, manual_ = false;
};
}  // namespace local_stop
