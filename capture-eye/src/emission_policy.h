#pragma once

#include <chrono>
#include <optional>

#include "track_message.h"

namespace capture_eye {

// Decides whether a track line goes on the wire this tick, and what it says.
//
// Pure state machine with the clock passed in, so every rule below is testable
// without sleeping. The rules exist because of how the firmware behaves:
//
//  - An explicit `lost` retires the target immediately, so it is sent once as
//    soon as the target is genuinely gone rather than waiting for the device's
//    own lost_ms timeout.
//  - `lost` then repeats slowly, because the device's rx queue is 8 deep and
//    drops silently (firmware/src/main.cpp:58); one dropped line should not
//    leave the device believing it still has a target.
//  - A short grace period absorbs a single missed inference so one bad frame
//    does not retire a good track.
//  - Nothing is sent before the first ever detection: there is no target to
//    retire, and the device sits at home on silence.
struct EmissionPolicy {
  int max_hz = 60;
  int lost_repeat_hz = 5;
  std::chrono::milliseconds lost_grace{200};

  struct Decision {
    std::optional<TrackMessage> message;
    bool forget_target = false;  // tells the selector to drop its lock
  };

  [[nodiscard]] Decision update(const std::optional<TrackMessage>& target,
                                std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> last_emit_;
  std::optional<std::chrono::steady_clock::time_point> last_seen_;
  bool ever_detected_ = false;
  bool lost_declared_ = false;
};

} // namespace capture_eye
