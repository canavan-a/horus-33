#include "audio_ring.h"

#include <utility>

namespace capture_eye {

void AudioRing::push(AudioPacket packet) {
  const std::scoped_lock lock{mutex_};
  const auto newest = packet.captured_at;
  packets_.push_back(std::move(packet));
  while (!packets_.empty() && newest - packets_.front().captured_at > window_) {
    packets_.pop_front();
  }
}

std::vector<AudioPacket> AudioRing::between(std::chrono::steady_clock::time_point after,
                                            std::chrono::steady_clock::time_point until) const {
  const std::scoped_lock lock{mutex_};
  std::vector<AudioPacket> out;
  for (const auto& packet : packets_) {
    if (packet.captured_at <= after) continue;
    if (packet.captured_at > until) break;  // in capture order, so nothing later qualifies
    out.push_back(packet);
  }
  return out;
}

} // namespace capture_eye
