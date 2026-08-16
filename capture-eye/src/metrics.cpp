#include "metrics.h"

#include <format>

namespace capture_eye {

std::string MetricsSnapshot::render() const {
  auto line = std::format(
      "capture {:3} fps | infer {:3} fps ({:.1f} ms) | render {:3} fps | track {:3} /s | "
      "skipped {:3} | lag {:.1f} ms",
      frames_captured, inferences, static_cast<double>(inference_latency_us) / 1000.0,
      frames_rendered, tracks_sent, inference_skipped,
      static_cast<double>(render_lag_us) / 1000.0);

  // Faults are appended only when non-zero, so a healthy line stays scannable
  // and anything unusual stands out instead of hiding among zeroes.
  if (overlay_dropped > 0) line += std::format(" | overlay-drop {}", overlay_dropped);
  if (pool_starved > 0) line += std::format(" | starved {}", pool_starved);
  if (decode_failures > 0) line += std::format(" | decode-fail {}", decode_failures);
  if (inference_failures > 0) line += std::format(" | infer-fail {}", inference_failures);
  if (sink_errors > 0) line += std::format(" | sink-err {}", sink_errors);
  if (track_errors > 0) line += std::format(" | track-err {}", track_errors);
  return line;
}

} // namespace capture_eye
