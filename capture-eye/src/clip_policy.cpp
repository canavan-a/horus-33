#include "clip_policy.h"

namespace capture_eye {

ClipPolicy::Decision ClipPolicy::update(bool person_present) {
  Decision decision;

  if (person_present) {
    no_person_ticks_ = 0;  // any present tick resets the countdown outright
    if (!recording_) {
      recording_ = true;
      decision.start_clip = true;
    }
    decision.recording = recording_;
    return decision;
  }

  if (recording_) {
    ++no_person_ticks_;
    if (no_person_ticks_ >= stop_after_ticks) {
      recording_ = false;
      no_person_ticks_ = 0;
      decision.stop_clip = true;
    }
  }
  decision.recording = recording_;
  return decision;
}

} // namespace capture_eye
