#pragma once

#include <optional>
#include <span>

#include "config.h"
#include "detection.h"

namespace capture_eye {

// Picks the one person the gimbal should follow.
//
// The default policy is sticky: the device runs a PID centring loop, and
// flapping between two people at inference rate makes it oscillate, which is
// worse than steadily tracking the "wrong" person. Continuity beats optimality.
struct TargetSelector {
  TargetPolicy policy = TargetPolicy::sticky_largest;
  float lock_iou = 0.3f;
  std::optional<BoxF> previous;

  [[nodiscard]] std::optional<Detection> select(std::span<const Detection> people);

  // Called when the target has been gone longer than the grace period, so
  // re-acquisition is not anchored to a stale box.
  void forget() { previous.reset(); }
};

} // namespace capture_eye
