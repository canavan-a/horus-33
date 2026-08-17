#pragma once

namespace capture_eye {

// Decides when a clip starts and stops, driven purely by a per-tick
// present/absent signal — no clock, unlike EmissionPolicy, because this
// hysteresis is a tick count, not a time window. Callers must call update()
// exactly once per distinct detection tick (not once per overlay frame,
// which repeats a stale tick when inference is slower than capture), so
// "N consecutive ticks" means N real inference results, not N video frames.
struct ClipPolicy {
  int stop_after_ticks = 40;

  struct Decision {
    bool start_clip = false;
    bool stop_clip = false;
    bool recording = false;  // state after this update, for status/telemetry
  };

  [[nodiscard]] Decision update(bool person_present);

private:
  bool recording_ = false;
  int no_person_ticks_ = 0;
};

} // namespace capture_eye
