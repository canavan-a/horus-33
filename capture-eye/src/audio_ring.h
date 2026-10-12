#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace capture_eye {

// One encoded audio frame (20ms of Opus), stamped on the same steady clock
// video frames use, so any consumer can line it up with video by subtracting
// its own epoch — the RTSP stream's start or a clip's first frame.
struct AudioPacket {
  std::vector<std::uint8_t> data;
  std::chrono::steady_clock::time_point captured_at{};  // the first sample's capture time
  std::int64_t duration_samples = 0;
};

// The last `window` of audio packets, filled by the audio thread and read by
// the clip sink on the overlay thread. Doubles as the clip's audio pre-roll:
// a clip that starts with a second of buffered video can pick up the audio
// that goes with it. Bounded by time, so memory stays flat — at 32kbps a few
// seconds of Opus is tens of KB.
class AudioRing {
public:
  explicit AudioRing(std::chrono::steady_clock::duration window) : window_{window} {}

  // Packets arrive in capture order; anything older than `window` before the
  // newest is dropped.
  void push(AudioPacket packet);

  // Packets with after < captured_at <= until, oldest first. A copy, so the
  // caller can mux them without holding the lock against the audio thread.
  [[nodiscard]] std::vector<AudioPacket> between(std::chrono::steady_clock::time_point after,
                                                 std::chrono::steady_clock::time_point until) const;

private:
  std::chrono::steady_clock::duration window_;
  mutable std::mutex mutex_;
  std::deque<AudioPacket> packets_;
};

} // namespace capture_eye
