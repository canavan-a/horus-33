#include "emission_policy.h"

#include <algorithm>

namespace capture_eye {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] Clock::duration interval_for(int hz) {
  const int safe_hz = std::max(1, hz);
  return std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{1}) / safe_hz;
}

} // namespace

EmissionPolicy::Decision EmissionPolicy::update(const std::optional<TrackMessage>& target,
                                                Clock::time_point now) {
  Decision decision;

  if (target.has_value() && !target->lost) {
    last_seen_ = now;
    ever_detected_ = true;
    lost_declared_ = false;

    // Rate cap. Inference is normally slower than this anyway; the cap only
    // matters if a faster backend lands.
    if (last_emit_.has_value() && now - *last_emit_ < interval_for(max_hz)) {
      return decision;
    }
    last_emit_ = now;
    decision.message = target;
    return decision;
  }

  // No target this tick.
  if (!ever_detected_) {
    return decision;  // nothing was ever acquired; stay silent
  }

  if (last_seen_.has_value() && now - *last_seen_ < lost_grace) {
    return decision;  // a single missed inference is not a lost target
  }

  if (!lost_declared_) {
    // First confirmed loss: tell the device at once so it retires the target.
    lost_declared_ = true;
    decision.forget_target = true;
    last_emit_ = now;
    decision.message = TrackMessage{.lost = true, .seq = std::nullopt};
    return decision;
  }

  // Still gone: repeat slowly so a dropped line cannot strand the device.
  if (last_emit_.has_value() && now - *last_emit_ < interval_for(lost_repeat_hz)) {
    return decision;
  }
  last_emit_ = now;
  decision.message = TrackMessage{.lost = true, .seq = std::nullopt};
  return decision;
}

} // namespace capture_eye
